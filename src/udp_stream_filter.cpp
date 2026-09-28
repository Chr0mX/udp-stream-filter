// udp_stream_filter.cpp -- OBS module entry points and the filter's own
// lifecycle/settings callbacks.
#include "udp_stream_filter.h"

#include <algorithm>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("xudp", "en-US")

static void udp_stream_update(void *data, obs_data_t *settings);

void draw_crop_overlay(float x, float y, float w, float h, float src_w, float src_h)
{
	if (w <= 0.0f || h <= 0.0f || src_w <= 0.0f || src_h <= 0.0f)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid)
		return;

	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	struct vec4 color;
	vec4_set(&color, 0.15f, 0.95f, 0.25f, 0.9f);
	gs_effect_set_vec4(color_param, &color);

	const float t = std::max(2.0f, std::min(src_w, src_h) * 0.004f);

	gs_projection_push();
	gs_ortho(0.0f, src_w, 0.0f, src_h, -100.0f, 100.0f);
	gs_matrix_push();
	gs_matrix_identity();

	auto draw_bar = [&](float bx, float by, float bw, float bh) {
		if (bw <= 0.0f || bh <= 0.0f)
			return;
		gs_matrix_push();
		gs_matrix_translate3f(bx, by, 0.0f);
		gs_matrix_scale3f(bw, bh, 1.0f);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, 1, 1);
		gs_matrix_pop();
	};

	draw_bar(x, y, w, t);
	draw_bar(x, y + h - t, w, t);
	draw_bar(x, y, t, h);
	draw_bar(x + w - t, y, t, h);

	gs_matrix_pop();
	gs_projection_pop();
}

static const char *udp_stream_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "UDP Stream (Colour)";
}

static void udp_stream_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	udp_stream_filter *f = (udp_stream_filter *)data;

	obs_source_t *target = obs_filter_get_target(f->source);

	bool udp_enabled;
	int max_fps;
	bool show_overlay;
	bool crop_enabled;
	int crop_width, crop_height, crop_anchor_x, crop_anchor_y;
	{
		std::lock_guard<std::mutex> net_lock(f->net_mtx);
		udp_enabled = f->udp_enabled;
		max_fps = f->max_fps;
		show_overlay = f->show_crop_overlay;
		crop_enabled = f->crop_enabled;
		crop_width = f->crop_width;
		crop_height = f->crop_height;
		crop_anchor_x = f->crop_anchor_x;
		crop_anchor_y = f->crop_anchor_y;
	}

	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	if (target && udp_enabled && width > 0 && height > 0) {
		bool should_capture = true;

		if (max_fps > 0) {
			auto now = std::chrono::steady_clock::now();
			auto min_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<double>(1.0 / (double)max_fps));

			if (!f->first_sent) {
				f->last_send = now;
				f->first_sent = true;
			} else if (now - f->last_send < min_interval) {
				should_capture = false;
			} else {
				f->last_send += min_interval;
				if (now - f->last_send > min_interval)
					f->last_send = now - min_interval;
			}
		}

		if (should_capture)
			capture_and_queue_frame(f, target, width, height);
	}

	// Crop overlay is drawn onto the preview/program output so anchors are
	// visible without guessing. Independent of whether streaming is on.
	if (target && show_overlay && crop_enabled && width > 0 && height > 0) {
		obs_source_video_render(target);
		CaptureRect rect = compute_capture_rect((int)width, (int)height, true, crop_width, crop_height,
							crop_anchor_x, crop_anchor_y);
		draw_crop_overlay((float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, (float)width,
				  (float)height);
		return;
	}

	obs_source_skip_video_filter(f->source);
}

static void *udp_stream_create(obs_data_t *settings, obs_source_t *source)
{
	udp_stream_filter *f = new udp_stream_filter();
	f->source = source;
	f->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	f->stagesurface[0] = nullptr;
	f->stagesurface[1] = nullptr;
	f->stage_width[0] = f->stage_width[1] = 0;
	f->stage_height[0] = f->stage_height[1] = 0;
	f->stage_valid[0] = f->stage_valid[1] = false;
	f->stage_write_idx = 0;
	f->sock = INVALID_SOCKET;
	f->addr_len = 0;
	f->applied_port = 0;
	f->frames_sent = 0;
	f->first_sent = false;
	f->last_send = std::chrono::steady_clock::now();
	f->fps_window_start = std::chrono::steady_clock::now();
	f->last_status_log = std::chrono::steady_clock::now();
	f->pending_count = 0;
	f->udp_payload_size = 1400;
	f->downscale_to = 0;
	f->show_crop_overlay = true;
	f->append_timestamp_trailer = true;

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

	if (f->sock != INVALID_SOCKET)
		closesocket(f->sock);

	obs_enter_graphics();
	if (f->stagesurface[0])
		gs_stagesurface_destroy(f->stagesurface[0]);
	if (f->stagesurface[1])
		gs_stagesurface_destroy(f->stagesurface[1]);
	if (f->texrender)
		gs_texrender_destroy(f->texrender);
	obs_leave_graphics();

	delete f;
}

static void udp_stream_update(void *data, obs_data_t *settings)
{
	udp_stream_filter *f = (udp_stream_filter *)data;

	bool new_udp_enabled = obs_data_get_bool(settings, "udp_enabled");
	std::string new_ip = obs_data_get_string(settings, "target_ip");
	int new_port = (int)obs_data_get_int(settings, "target_port");
	int new_jpeg_quality = (int)obs_data_get_int(settings, "jpeg_quality");
	int new_max_fps = (int)obs_data_get_int(settings, "max_fps");
	int new_crop_anchor_x = (int)obs_data_get_int(settings, "crop_anchor_x");
	int new_crop_anchor_y = (int)obs_data_get_int(settings, "crop_anchor_y");
	int new_output_preset = (int)obs_data_get_int(settings, "output_preset");
	int new_downscale_to = (int)obs_data_get_int(settings, "downscale_to");
	int new_payload = (int)obs_data_get_int(settings, "udp_payload_size");
	bool new_show_overlay = obs_data_get_bool(settings, "show_crop_overlay");
	bool new_append_trailer = obs_data_get_bool(settings, "append_timestamp_trailer");

	int preset_size = preset_crop_size(new_output_preset);

	bool new_crop_enabled;
	int new_crop_width;
	int new_crop_height;
	if (preset_size > 0) {
		new_crop_enabled = true;
		new_crop_width = preset_size;
		new_crop_height = preset_size;
	} else {
		new_crop_enabled = obs_data_get_bool(settings, "crop_enabled");
		new_crop_width = (int)obs_data_get_int(settings, "crop_width");
		new_crop_height = (int)obs_data_get_int(settings, "crop_height");
	}

	std::lock_guard<std::mutex> net_lock(f->net_mtx);

	const bool was_enabled = f->udp_enabled;

	f->udp_enabled = new_udp_enabled;
	f->jpeg_quality = new_jpeg_quality;
	f->max_fps = new_max_fps;
	f->crop_anchor_x = new_crop_anchor_x;
	f->crop_anchor_y = new_crop_anchor_y;
	f->output_preset = new_output_preset;
	f->crop_enabled = new_crop_enabled;
	f->crop_width = new_crop_width;
	f->crop_height = new_crop_height;
	f->downscale_to = new_downscale_to > 0 ? new_downscale_to : 0;
	f->udp_payload_size = (int)clamp_udp_payload(new_payload);
	f->show_crop_overlay = new_show_overlay;
	f->append_timestamp_trailer = new_append_trailer;

	// Always keep the typed host/port on the struct (for Apply + stats),
	// but do NOT recreate the socket on every keystroke -- that was racing
	// the encode thread and briefly interrupting the stream while typing.
	f->target_ip = new_ip;
	f->target_port = new_port;

	if (f->udp_enabled) {
		// Create/rebuild only when streaming is turned on (or was on
		// but never successfully bound). Host edits require Apply.
		if (!was_enabled || f->sock == INVALID_SOCKET)
			setup_socket_locked(f);
	} else if (f->sock != INVALID_SOCKET) {
		closesocket(f->sock);
		f->sock = INVALID_SOCKET;
	}
}

#ifdef _WIN32
static bool g_wsa_started = false;
#endif

bool obs_module_load(void)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
		g_wsa_started = true;
	} else {
		blog(LOG_ERROR, "[xudp] WSAStartup failed");
	}
#endif

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
#ifdef _WIN32
	if (g_wsa_started)
		WSACleanup();
#endif
}
