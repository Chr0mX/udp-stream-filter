// udp_stream_filter.cpp -- OBS module entry points and the filter's own
// lifecycle/settings callbacks. The per-frame render path, the socket/wire
// protocol, and the properties UI now live in their own translation units
// (udp_stream_capture.cpp / udp_stream_net.cpp / udp_stream_properties.cpp)
// -- see udp_stream_filter.h for the struct definition and the declarations
// shared across them.
#include "udp_stream_filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("xudp", "en-US")

static void udp_stream_update(void *data, obs_data_t *settings);

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

	// Snapshot udp_enabled/max_fps under net_mtx instead of reading
	// f->udp_enabled/f->max_fps directly -- both are written by
	// udp_stream_update() (the settings-update callback) as part of one
	// locked group, and this render-thread read must join that same
	// group instead of racing it. See udp_stream_filter.h's comment on
	// the settings block.
	bool udp_enabled;
	int max_fps;
	{
		std::lock_guard<std::mutex> net_lock(f->net_mtx);
		udp_enabled = f->udp_enabled;
		max_fps = f->max_fps;
	}

	if (!target || !udp_enabled) {
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
			// Advance the schedule by exactly one interval (banking any
			// leftover time) instead of snapping to "now". Snapping to
			// "now" always floors the achievable rate to an exact integer
			// divisor of the render callback's tick rate (e.g. requesting
			// 140fps against a 240Hz render loop always lands on 120,
			// never 140) because it throws away the fractional progress
			// made toward the next frame. Banking it lets the gate "catch
			// up" over multiple ticks so the long-run average converges
			// on the actual requested max_fps.
			f->last_send += min_interval;

			// If we've fallen far behind (e.g. after a stall/hitch),
			// don't let the bank grow unbounded -- that would fire a
			// burst of back-to-back catch-up frames. Clamp to at most
			// one interval behind now.
			if (now - f->last_send > min_interval)
				f->last_send = now - min_interval;
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
	f->stagesurface[0] = nullptr;
	f->stagesurface[1] = nullptr;
	f->stage_width[0] = f->stage_width[1] = 0;
	f->stage_height[0] = f->stage_height[1] = 0;
	f->stage_valid[0] = f->stage_valid[1] = false;
	f->stage_write_idx = 0;
	f->sock = INVALID_SOCKET;
	f->frames_sent = 0;
	f->first_sent = false;
	f->last_send = std::chrono::steady_clock::now();
	f->fps_window_start = std::chrono::steady_clock::now();
	f->last_ui_refresh = std::chrono::steady_clock::now();

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

	// Read every setting into a local first -- none of this needs a lock,
	// it's just obs_data_t access -- then take net_mtx exactly once below
	// to publish the whole group atomically. This replaces the previous
	// field-by-field unlocked writes (with only jpeg_quality individually
	// guarded): the render thread reads several of these fields every
	// frame (see udp_stream_video_render()/capture_and_queue_frame()), so
	// writing them unlocked was a real, if practically benign on x86/x64,
	// data race -- and the five crop-rect fields specifically must change
	// together as one group or a reader can observe a torn mix of old and
	// new values.
	bool new_udp_enabled = obs_data_get_bool(settings, "udp_enabled");
	std::string new_ip = obs_data_get_string(settings, "target_ip");
	int new_port = (int)obs_data_get_int(settings, "target_port");
	int new_jpeg_quality = (int)obs_data_get_int(settings, "jpeg_quality");
	int new_max_fps = (int)obs_data_get_int(settings, "max_fps");
	int new_crop_anchor_x = (int)obs_data_get_int(settings, "crop_anchor_x");
	int new_crop_anchor_y = (int)obs_data_get_int(settings, "crop_anchor_y");
	int new_output_preset = (int)obs_data_get_int(settings, "output_preset");

	int preset_size = new_output_preset == 1 ? 320 : new_output_preset == 2 ? 640 : 0;

	bool new_crop_enabled;
	int new_crop_width;
	int new_crop_height;
	if (preset_size > 0) {
		// Presets force a native square crop at the given size; the
		// anchor remains user-adjustable (defaults to center).
		new_crop_enabled = true;
		new_crop_width = preset_size;
		new_crop_height = preset_size;
	} else {
		new_crop_enabled = obs_data_get_bool(settings, "crop_enabled");
		new_crop_width = (int)obs_data_get_int(settings, "crop_width");
		new_crop_height = (int)obs_data_get_int(settings, "crop_height");
	}

	// Everything from here down is one locked group: the settings fields
	// themselves, plus the socket lifecycle (which depends on whether
	// udp_enabled/target_ip/target_port changed) -- the encode thread may
	// be inside send_jpeg_chunked() on the old socket/address right now,
	// so both the field writes and the socket teardown/setup need to be
	// atomic with respect to it.
	std::lock_guard<std::mutex> net_lock(f->net_mtx);

	f->udp_enabled = new_udp_enabled;
	f->jpeg_quality = new_jpeg_quality;
	f->max_fps = new_max_fps;
	f->crop_anchor_x = new_crop_anchor_x;
	f->crop_anchor_y = new_crop_anchor_y;
	f->output_preset = new_output_preset;
	f->crop_enabled = new_crop_enabled;
	f->crop_width = new_crop_width;
	f->crop_height = new_crop_height;

	bool ip_or_port_changed = (new_ip != f->target_ip) || (new_port != f->target_port);
	f->target_ip = new_ip;
	f->target_port = new_port;

	if (f->udp_enabled && (f->sock == INVALID_SOCKET || ip_or_port_changed)) {
		setup_socket_locked(f);
	} else if (!f->udp_enabled && f->sock != INVALID_SOCKET) {
		closesocket(f->sock);
		f->sock = INVALID_SOCKET;
	}
}

// ---------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------

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
