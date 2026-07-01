#include <obs-module.h>
#include <obs-source.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <opencv2/opencv.hpp>
#include <turbojpeg.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#undef min
#undef max
#endif

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("xudp", "en-US")

static void udp_stream_update(void *data, obs_data_t *settings);

struct udp_stream_filter {
    obs_source_t *source;
    gs_texrender_t *texrender;
    gs_stagesurf_t *stagesurface;

    bool udp_enabled;
    std::string target_ip;
    int target_port;
    int jpeg_quality;
    int max_fps;
    bool crop_zoom_output;
    int output_preset;
    int fov;
    float zoom_level;
    int out_w;
    int out_h;
    int crop_mode;
    int crop_off_x;
    int crop_off_y;
    bool keep_aspect;

    bool enable_color_boost;
    bool boost_yellow;
    bool boost_purple;
    bool boost_red;
    float boost_str;
    bool suppress_bg;
    bool adaptive_boost;

    SOCKET sock;
    sockaddr_in addr;
    std::chrono::steady_clock::time_point last_send;
    int frames_sent;
    bool first_sent;

    tjhandle tj;

    std::thread enc_thread;
    std::atomic<bool> enc_running{false};
    std::mutex enc_mtx;
    std::condition_variable enc_cv;
    cv::Mat pending;
    bool has_pending{false};
};

// ---------------------------------------------------------------------------
// Crop / zoom / FOV math
// ---------------------------------------------------------------------------

// Converts a "simulated FOV" in degrees into an equivalent zoom multiplier,
// relative to an assumed baseline FOV (90 degrees is a common camera default).
// Returns -1.0f to signal "not in use" (fov <= 0), so the caller can fall back
// to the explicit zoom_level setting.
static float fov_to_zoom(int fov_deg, int base_fov_deg = 90)
{
    if (fov_deg <= 0)
        return -1.0f;

    float fov_rad = (float)fov_deg * (float)M_PI / 180.0f;
    float base_rad = (float)base_fov_deg * (float)M_PI / 180.0f;

    return tanf(base_rad / 2.0f) / tanf(fov_rad / 2.0f);
}

// Computes the source-space crop rectangle given the current zoom/FOV and
// crop offset settings. zoom > 1.0 shrinks the crop region (zooms in).
static cv::Rect compute_crop_rect(int src_w, int src_h, udp_stream_filter *f)
{
    float zoom = fov_to_zoom(f->fov);
    if (zoom < 0.0f)
        zoom = f->zoom_level > 0.01f ? f->zoom_level : 1.0f;

    float crop_w = (float)src_w / zoom;
    float crop_h = (float)src_h / zoom;

    if (f->keep_aspect && f->out_w > 0 && f->out_h > 0) {
        float target_aspect = (float)f->out_w / (float)f->out_h;
        float crop_aspect = crop_w / crop_h;

        if (crop_aspect > target_aspect)
            crop_w = crop_h * target_aspect;
        else
            crop_h = crop_w / target_aspect;
    }

    crop_w = std::min(crop_w, (float)src_w);
    crop_h = std::min(crop_h, (float)src_h);
    crop_w = std::max(crop_w, 2.0f);
    crop_h = std::max(crop_h, 2.0f);

    float cx, cy;
    if (f->crop_mode == 1) {
        // Custom offset, relative to frame center.
        cx = src_w / 2.0f + (float)f->crop_off_x;
        cy = src_h / 2.0f + (float)f->crop_off_y;
    } else {
        // Center crop.
        cx = src_w / 2.0f;
        cy = src_h / 2.0f;
    }

    float x = cx - crop_w / 2.0f;
    float y = cy - crop_h / 2.0f;

    // Clamp so the crop rect stays fully inside the source frame.
    x = std::max(0.0f, std::min(x, (float)src_w - crop_w));
    y = std::max(0.0f, std::min(y, (float)src_h - crop_h));

    return cv::Rect((int)std::round(x), (int)std::round(y), (int)std::round(crop_w),
                     (int)std::round(crop_h));
}

// Applies crop/zoom (if enabled) and resizes to the configured output
// resolution. Always returns an owned (deep-copied) Mat.
static cv::Mat process_crop_zoom(udp_stream_filter *f, const cv::Mat &src)
{
    int out_w = f->out_w > 0 ? f->out_w : src.cols;
    int out_h = f->out_h > 0 ? f->out_h : src.rows;

    cv::Mat working = src;

    if (f->crop_zoom_output) {
        cv::Rect r = compute_crop_rect(src.cols, src.rows, f);
        working = src(r);
    }

    if (working.cols == out_w && working.rows == out_h)
        return working.clone();

    cv::Mat resized;
    cv::resize(working, resized, cv::Size(out_w, out_h), 0, 0, cv::INTER_LINEAR);
    return resized;
}

// ---------------------------------------------------------------------------
// UDP socket + chunked send
// ---------------------------------------------------------------------------

// Wire header for each UDP packet (16 bytes, all fields big-endian):
//   frame_id     (4 bytes) - increments per source frame, lets receiver detect drops/reorders
//   total_size   (4 bytes) - total JPEG size in bytes across all chunks of this frame
//   chunk_index  (2 bytes) - 0-based index of this chunk
//   total_chunks (2 bytes) - total number of chunks for this frame
//   chunk_size   (2 bytes) - payload bytes carried in this packet
static const size_t UDP_HEADER_SIZE = 16;
static const size_t UDP_MAX_PAYLOAD = 60000; // LAN-only: let IP fragmentation handle MTU,
                                              // most frames fit in 1-2 datagrams instead of ~45

static bool setup_socket(udp_stream_filter *f)
{
    if (f->sock != INVALID_SOCKET) {
        closesocket(f->sock);
        f->sock = INVALID_SOCKET;
    }

    f->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (f->sock == INVALID_SOCKET) {
        blog(LOG_ERROR, "[xudp] failed to create socket: %d", WSAGetLastError());
        return false;
    }

    memset(&f->addr, 0, sizeof(f->addr));
    f->addr.sin_family = AF_INET;
    f->addr.sin_port = htons((u_short)f->target_port);

    if (inet_pton(AF_INET, f->target_ip.c_str(), &f->addr.sin_addr) != 1) {
        blog(LOG_ERROR, "[xudp] invalid target IP: %s", f->target_ip.c_str());
        closesocket(f->sock);
        f->sock = INVALID_SOCKET;
        return false;
    }

    // Larger send buffer so bursts of big (multi-chunk) frames don't get
    // dropped at the socket layer before reaching the wire.
    int sndbuf = 1 * 1024 * 1024;
    setsockopt(f->sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

    return true;
}

static void send_jpeg_chunked(udp_stream_filter *f, const uint8_t *jpeg, unsigned long jpeg_size,
                               uint32_t frame_id)
{
    if (f->sock == INVALID_SOCKET || jpeg_size == 0)
        return;

    uint16_t total_chunks = (uint16_t)((jpeg_size + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
    if (total_chunks == 0)
        total_chunks = 1;

    std::vector<uint8_t> packet(UDP_HEADER_SIZE + UDP_MAX_PAYLOAD);
    size_t offset = 0;

    for (uint16_t i = 0; i < total_chunks; i++) {
        size_t remaining = jpeg_size - offset;
        size_t this_size = std::min(remaining, UDP_MAX_PAYLOAD);

        uint32_t n_frame_id = htonl(frame_id);
        uint32_t n_total_size = htonl((uint32_t)jpeg_size);
        uint16_t n_chunk_index = htons(i);
        uint16_t n_total_chunks = htons(total_chunks);
        uint16_t n_chunk_size = htons((uint16_t)this_size);

        size_t p = 0;
        memcpy(&packet[p], &n_frame_id, 4); p += 4;
        memcpy(&packet[p], &n_total_size, 4); p += 4;
        memcpy(&packet[p], &n_chunk_index, 2); p += 2;
        memcpy(&packet[p], &n_total_chunks, 2); p += 2;
        memcpy(&packet[p], &n_chunk_size, 2); p += 2;
        memcpy(&packet[p], jpeg + offset, this_size); p += this_size;

        int sent = sendto(f->sock, (const char *)packet.data(), (int)p, 0,
                           (const sockaddr *)&f->addr, sizeof(f->addr));

        if (sent == SOCKET_ERROR) {
            blog(LOG_WARNING, "[xudp] sendto failed: %d", WSAGetLastError());
            break;
        }

        offset += this_size;
    }
}

// ---------------------------------------------------------------------------
// Background JPEG encode thread
// ---------------------------------------------------------------------------

static void encode_thread_func(udp_stream_filter *f)
{
    while (f->enc_running) {
        cv::Mat frame;
        uint32_t frame_id;

        {
            std::unique_lock<std::mutex> lock(f->enc_mtx);
            f->enc_cv.wait(lock, [f] { return f->has_pending || !f->enc_running; });

            if (!f->enc_running)
                break;

            frame = f->pending.clone();
            f->has_pending = false;
            frame_id = (uint32_t)f->frames_sent;
        }

        if (frame.empty() || !f->tj)
            continue;

        unsigned char *jpeg_buf = nullptr;
        unsigned long jpeg_size = 0;

        int ret = tjCompress2(f->tj, frame.data, frame.cols, (int)frame.step, frame.rows,
                               TJPF_BGR, &jpeg_buf, &jpeg_size, TJSAMP_420, f->jpeg_quality,
                               TJFLAG_FASTDCT);

        if (ret == 0 && jpeg_buf && jpeg_size > 0) {
            send_jpeg_chunked(f, jpeg_buf, jpeg_size, frame_id);
            f->frames_sent++;
        } else {
            blog(LOG_WARNING, "[xudp] JPEG compression failed");
        }

        if (jpeg_buf)
            tjFree(jpeg_buf);
    }
}

// ---------------------------------------------------------------------------
// Frame capture (GPU texture -> CPU Mat) and queueing
// ---------------------------------------------------------------------------

static void capture_and_queue_frame(udp_stream_filter *f, obs_source_t *target, uint32_t width,
                                     uint32_t height)
{
    gs_texrender_reset(f->texrender);

    if (!gs_texrender_begin(f->texrender, width, height))
        return;

    struct vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

    obs_source_video_render(target);

    gs_texrender_end(f->texrender);

    gs_texture_t *tex = gs_texrender_get_texture(f->texrender);
    if (!tex)
        return;

    if (!f->stagesurface || gs_stagesurface_get_width(f->stagesurface) != width ||
        gs_stagesurface_get_height(f->stagesurface) != height) {
        if (f->stagesurface)
            gs_stagesurface_destroy(f->stagesurface);
        f->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
    }

    gs_stage_texture(f->stagesurface, tex);

    uint8_t *mapped_data = nullptr;
    uint32_t linesize = 0;
    if (!gs_stagesurface_map(f->stagesurface, &mapped_data, &linesize))
        return;

    cv::Mat bgra((int)height, (int)width, CV_8UC4, mapped_data, linesize);
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);

    gs_stagesurface_unmap(f->stagesurface);

    cv::Mat out_frame = process_crop_zoom(f, bgr);

    {
        std::lock_guard<std::mutex> lock(f->enc_mtx);
        f->pending = out_frame;
        f->has_pending = true;
    }
    f->enc_cv.notify_one();
}

// ---------------------------------------------------------------------------
// OBS filter callbacks
// ---------------------------------------------------------------------------

static const char *udp_stream_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Colour";
}

static void udp_stream_video_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    udp_stream_filter *f = (udp_stream_filter *)data;

    obs_source_t *target = obs_filter_get_target(f->source);
    if (!target || !f->udp_enabled) {
        obs_source_skip_video_filter(f->source);
        return;
    }

    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);

    if (width == 0 || height == 0) {
        obs_source_skip_video_filter(f->source);
        return;
    }

    bool should_capture = true;

    if (f->max_fps > 0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - f->last_send).count();
        double min_interval = 1.0 / (double)f->max_fps;

        if (f->first_sent && elapsed < min_interval) {
            should_capture = false;
        } else {
            f->last_send = now;
            f->first_sent = true;
        }
    }

    if (should_capture)
        capture_and_queue_frame(f, target, width, height);

    // This filter never modifies the displayed/output video; it only taps a
    // copy of it for streaming. Pass the original render straight through.
    obs_source_skip_video_filter(f->source);
}

static void *udp_stream_create(obs_data_t *settings, obs_source_t *source)
{
    udp_stream_filter *f = new udp_stream_filter();
    f->source = source;
    f->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    f->stagesurface = nullptr;
    f->sock = INVALID_SOCKET;
    f->frames_sent = 0;
    f->first_sent = false;
    f->last_send = std::chrono::steady_clock::now();

    f->tj = tjInitCompress();
    if (!f->tj)
        blog(LOG_ERROR, "[xudp] tjInitCompress failed");

    f->enc_running = true;
    f->enc_thread = std::thread(encode_thread_func, f);

    udp_stream_update(f, settings);

    return f;
}

static void udp_stream_destroy(void *data)
{
    udp_stream_filter *f = (udp_stream_filter *)data;

    f->enc_running = false;
    f->enc_cv.notify_all();
    if (f->enc_thread.joinable())
        f->enc_thread.join();

    if (f->tj)
        tjDestroy(f->tj);

    if (f->sock != INVALID_SOCKET)
        closesocket(f->sock);

    obs_enter_graphics();
    if (f->stagesurface)
        gs_stagesurface_destroy(f->stagesurface);
    if (f->texrender)
        gs_texrender_destroy(f->texrender);
    obs_leave_graphics();

    delete f;
}

static void udp_stream_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "udp_enabled", true);
    obs_data_set_default_string(settings, "target_ip", "127.0.0.1");
    obs_data_set_default_int(settings, "target_port", 5600);
    obs_data_set_default_int(settings, "jpeg_quality", 80);
    obs_data_set_default_int(settings, "max_fps", 0);
    obs_data_set_default_bool(settings, "crop_zoom_output", false);
    obs_data_set_default_int(settings, "output_preset", 0);
    obs_data_set_default_int(settings, "fov", 0);
    obs_data_set_default_double(settings, "zoom_level", 1.0);
    obs_data_set_default_int(settings, "out_w", 1920);
    obs_data_set_default_int(settings, "out_h", 1080);
    obs_data_set_default_int(settings, "crop_mode", 0);
    obs_data_set_default_int(settings, "crop_off_x", 0);
    obs_data_set_default_int(settings, "crop_off_y", 0);
    obs_data_set_default_bool(settings, "keep_aspect", true);
}

static void udp_stream_update(void *data, obs_data_t *settings)
{
    udp_stream_filter *f = (udp_stream_filter *)data;

    f->udp_enabled = obs_data_get_bool(settings, "udp_enabled");

    std::string new_ip = obs_data_get_string(settings, "target_ip");
    int new_port = (int)obs_data_get_int(settings, "target_port");

    f->jpeg_quality = (int)obs_data_get_int(settings, "jpeg_quality");
    f->max_fps = (int)obs_data_get_int(settings, "max_fps");
    f->fov = (int)obs_data_get_int(settings, "fov");
    f->zoom_level = (float)obs_data_get_double(settings, "zoom_level");
    f->crop_off_x = (int)obs_data_get_int(settings, "crop_off_x");
    f->crop_off_y = (int)obs_data_get_int(settings, "crop_off_y");

    f->output_preset = (int)obs_data_get_int(settings, "output_preset");
    int preset_size = f->output_preset == 1 ? 320 : f->output_preset == 2 ? 640 : 0;

    if (preset_size > 0) {
        // Presets force a square center-crop at the given size; zoom
        // amount and crop offset remain user-adjustable.
        f->crop_zoom_output = true;
        f->out_w = preset_size;
        f->out_h = preset_size;
        f->crop_mode = 0;
        f->keep_aspect = true;
    } else {
        f->crop_zoom_output = obs_data_get_bool(settings, "crop_zoom_output");
        f->out_w = (int)obs_data_get_int(settings, "out_w");
        f->out_h = (int)obs_data_get_int(settings, "out_h");
        f->crop_mode = (int)obs_data_get_int(settings, "crop_mode");
        f->keep_aspect = obs_data_get_bool(settings, "keep_aspect");
    }

    bool ip_or_port_changed = (new_ip != f->target_ip) || (new_port != f->target_port);
    f->target_ip = new_ip;
    f->target_port = new_port;

    if (f->udp_enabled && (f->sock == INVALID_SOCKET || ip_or_port_changed)) {
        setup_socket(f);
    } else if (!f->udp_enabled && f->sock != INVALID_SOCKET) {
        closesocket(f->sock);
        f->sock = INVALID_SOCKET;
    }
}

// Grays out the manual size/crop-mode controls whenever a standard output
// preset (320x320 / 640x640) is active, since their effective values are
// then dictated by the preset rather than user input.
static bool udp_stream_preset_modified(obs_properties_t *props, obs_property_t *property,
                                        obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    bool is_preset = obs_data_get_int(settings, "output_preset") != 0;

    obs_property_set_enabled(obs_properties_get(props, "crop_zoom_output"), !is_preset);
    obs_property_set_enabled(obs_properties_get(props, "out_w"), !is_preset);
    obs_property_set_enabled(obs_properties_get(props, "out_h"), !is_preset);
    obs_property_set_enabled(obs_properties_get(props, "crop_mode"), !is_preset);
    obs_property_set_enabled(obs_properties_get(props, "keep_aspect"), !is_preset);

    return true;
}

static obs_properties_t *udp_stream_get_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_bool(props, "udp_enabled", "Enable UDP Streaming");
    obs_properties_add_text(props, "target_ip", "Target IP", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "target_port", "Target Port", 1, 65535, 1);
    obs_properties_add_int_slider(props, "jpeg_quality", "JPEG Quality", 1, 100, 1);
    obs_properties_add_int(props, "max_fps", "Max FPS (0 = uncapped)", 0, 240, 1);

    obs_property_t *preset_list = obs_properties_add_list(
        props, "output_preset", "Output Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(preset_list, "Custom", 0);
    obs_property_list_add_int(preset_list, "320x320", 1);
    obs_property_list_add_int(preset_list, "640x640", 2);
    obs_property_set_modified_callback(preset_list, udp_stream_preset_modified);

    obs_properties_add_bool(props, "crop_zoom_output", "Enable Crop/Zoom");
    obs_properties_add_int(props, "fov", "Simulated FOV, deg (0 = use Zoom Level)", 0, 179, 1);
    obs_properties_add_float(props, "zoom_level", "Zoom Level", 1.0, 10.0, 0.1);
    obs_properties_add_int(props, "out_w", "Output Width", 16, 7680, 2);
    obs_properties_add_int(props, "out_h", "Output Height", 16, 4320, 2);

    obs_property_t *crop_mode_list = obs_properties_add_list(
        props, "crop_mode", "Crop Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(crop_mode_list, "Center", 0);
    obs_property_list_add_int(crop_mode_list, "Custom Offset", 1);

    obs_properties_add_int(props, "crop_off_x", "Crop Offset X (px)", -7680, 7680, 1);
    obs_properties_add_int(props, "crop_off_y", "Crop Offset Y (px)", -4320, 4320, 1);
    obs_properties_add_bool(props, "keep_aspect", "Keep Aspect Ratio");

    if (data) {
        udp_stream_filter *f = (udp_stream_filter *)data;
        obs_data_t *settings = obs_source_get_settings(f->source);
        udp_stream_preset_modified(props, preset_list, settings);
        obs_data_release(settings);
    }

    return props;
}

// ---------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------

static bool g_wsa_started = false;

bool obs_module_load(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        g_wsa_started = true;
    } else {
        blog(LOG_ERROR, "[xudp] WSAStartup failed");
    }

    struct obs_source_info udp_stream_filter_info = {};
    udp_stream_filter_info.id = "udp_stream_filter";
    udp_stream_filter_info.type = OBS_SOURCE_TYPE_FILTER;
    udp_stream_filter_info.output_flags = OBS_SOURCE_VIDEO;
    udp_stream_filter_info.get_name = udp_stream_get_name;
    udp_stream_filter_info.create = udp_stream_create;
    udp_stream_filter_info.destroy = udp_stream_destroy;
    udp_stream_filter_info.update = udp_stream_update;
    udp_stream_filter_info.get_defaults = udp_stream_get_defaults;
    udp_stream_filter_info.get_properties = udp_stream_get_properties;
    udp_stream_filter_info.video_render = udp_stream_video_render;

    obs_register_source(&udp_stream_filter_info);
    return true;
}

void obs_module_unload(void)
{
    if (g_wsa_started)
        WSACleanup();
}
