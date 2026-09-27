// udp_stream_properties.cpp -- OBS Filters-dialog properties: defaults,
// the widget list, and modified/button callbacks.
#include "udp_stream_filter.h"

#include <cstdio>

void udp_stream_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "udp_enabled", false);
	obs_data_set_default_string(settings, "target_ip", "127.0.0.1");
	obs_data_set_default_int(settings, "target_port", 5600);
	obs_data_set_default_int(settings, "jpeg_quality", 80);
	obs_data_set_default_int(settings, "max_fps", 120);
	obs_data_set_default_bool(settings, "crop_enabled", false);
	obs_data_set_default_int(settings, "output_preset", 0);
	obs_data_set_default_int(settings, "crop_width", 320);
	obs_data_set_default_int(settings, "crop_height", 320);
	obs_data_set_default_int(settings, "crop_anchor_x", -1);
	obs_data_set_default_int(settings, "crop_anchor_y", -1);
	obs_data_set_default_int(settings, "downscale_to", 0);
	obs_data_set_default_int(settings, "udp_payload_size", 1400);
	obs_data_set_default_bool(settings, "show_crop_overlay", true);
	obs_data_set_default_bool(settings, "append_timestamp_trailer", true);
}

static bool udp_stream_preset_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool is_preset = obs_data_get_int(settings, "output_preset") != 0;

	obs_property_set_enabled(obs_properties_get(props, "crop_enabled"), !is_preset);
	obs_property_set_enabled(obs_properties_get(props, "crop_width"), !is_preset);
	obs_property_set_enabled(obs_properties_get(props, "crop_height"), !is_preset);

	return true;
}

static bool udp_stream_refresh_stats(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	return true;
}

static bool udp_stream_apply_network(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (!data)
		return false;

	udp_stream_filter *f = (udp_stream_filter *)data;
	std::lock_guard<std::mutex> net_lock(f->net_mtx);
	if (f->udp_enabled) {
		setup_socket_locked(f);
	} else {
		// Validate/resolve even when not streaming so the user gets
		// immediate feedback on a bad host before enabling.
		if (setup_socket_locked(f) && f->sock != INVALID_SOCKET) {
			closesocket(f->sock);
			f->sock = INVALID_SOCKET;
		}
	}
	return true;
}

obs_properties_t *udp_stream_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, "udp_enabled", "Enable UDP Streaming");
	obs_properties_add_text(props, "target_ip", "Target Host (IP or hostname)", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "target_port", "Target Port", 1, 65535, 1);
	// button2 so the callback receives our filter pointer (not the source).
	obs_properties_add_button2(props, "apply_network", "Apply Target Address", udp_stream_apply_network, data);

	obs_property_t *payload_list = obs_properties_add_list(props, "udp_payload_size", "UDP Payload Size",
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(payload_list, "1200 (safe Wi-Fi)", 1200);
	obs_property_list_add_int(payload_list, "1400 (Ethernet MTU)", 1400);
	obs_property_list_add_int(payload_list, "8000 (jumbo-ish LAN)", 8000);
	obs_property_list_add_int(payload_list, "60000 (LAN + IP fragmentation)", 60000);

	obs_properties_add_int_slider(props, "jpeg_quality", "JPEG Quality", 1, 100, 1);
	obs_properties_add_int(props, "max_fps", "Max FPS (0 = uncapped)", 0, 240, 1);
	obs_properties_add_bool(props, "append_timestamp_trailer", "Append XUDP timestamp trailer (compat)");

	obs_properties_add_text(props, "stream_fps_debug", "Stream Stats", OBS_TEXT_INFO);
	obs_properties_add_button(props, "refresh_stream_stats", "Refresh Stream Stats", udp_stream_refresh_stats);

	obs_property_t *preset_list = obs_properties_add_list(props, "output_preset", "Output Preset",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preset_list, "Custom", 0);
	obs_property_list_add_int(preset_list, "160x160", 1);
	obs_property_list_add_int(preset_list, "320x320", 2);
	obs_property_list_add_int(preset_list, "416x416", 3);
	obs_property_list_add_int(preset_list, "512x512", 4);
	obs_property_list_add_int(preset_list, "640x640", 5);
	obs_property_set_modified_callback(preset_list, udp_stream_preset_modified);

	obs_properties_add_bool(props, "crop_enabled", "Enable Crop");
	obs_properties_add_int(props, "crop_width", "Crop Width (px)", 1, 7680, 1);
	obs_properties_add_int(props, "crop_height", "Crop Height (px)", 1, 4320, 1);
	obs_properties_add_int(props, "crop_anchor_x", "Crop Anchor X, px (-1 = center)", -1, 7680, 1);
	obs_properties_add_int(props, "crop_anchor_y", "Crop Anchor Y, px (-1 = center)", -1, 4320, 1);
	obs_properties_add_bool(props, "show_crop_overlay", "Show Crop Overlay on Preview");

	obs_property_t *scale_list = obs_properties_add_list(props, "downscale_to", "Encode Size (after crop)",
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(scale_list, "Native (no downscale)", 0);
	obs_property_list_add_int(scale_list, "160x160", 160);
	obs_property_list_add_int(scale_list, "320x320", 320);
	obs_property_list_add_int(scale_list, "416x416", 416);
	obs_property_list_add_int(scale_list, "512x512", 512);
	obs_property_list_add_int(scale_list, "640x640", 640);

	if (data) {
		udp_stream_filter *f = (udp_stream_filter *)data;
		obs_data_t *settings = obs_source_get_settings(f->source);
		udp_stream_preset_modified(props, preset_list, settings);

		char fps_buf[320];
		double fps = f->measured_fps.load();
		double kbps = f->measured_kbps.load();
		uint64_t dropped, send_errors;
		uint32_t last_jpeg;
		uint16_t last_chunks;
		{
			std::lock_guard<std::mutex> lock(f->enc_mtx);
			dropped = f->frames_dropped;
			send_errors = f->send_errors;
			last_jpeg = f->last_jpeg_size;
			last_chunks = f->last_chunk_count;
		}

		bool udp_enabled;
		std::string sock_err;
		std::string applied_ip;
		int applied_port;
		{
			std::lock_guard<std::mutex> net_lock(f->net_mtx);
			udp_enabled = f->udp_enabled;
			sock_err = f->last_socket_error;
			applied_ip = f->applied_ip;
			applied_port = f->applied_port;
		}

		if (udp_enabled && fps > 0.0) {
			snprintf(fps_buf, sizeof(fps_buf),
				 "%.1f fps | %.1f kbps | jpeg %u B / %u chunks | dropped %llu | "
				 "send_err %llu | applied %s:%d%s%s",
				 fps, kbps, last_jpeg, (unsigned)last_chunks, (unsigned long long)dropped,
				 (unsigned long long)send_errors, applied_ip.empty() ? "?" : applied_ip.c_str(),
				 applied_port, sock_err.empty() ? "" : " | ", sock_err.empty() ? "" : sock_err.c_str());
		} else if (udp_enabled) {
			snprintf(fps_buf, sizeof(fps_buf), "starting... | applied %s:%d%s%s",
				 applied_ip.empty() ? "(none)" : applied_ip.c_str(), applied_port,
				 sock_err.empty() ? "" : " | ", sock_err.empty() ? "" : sock_err.c_str());
		} else {
			snprintf(fps_buf, sizeof(fps_buf), "-- (not streaming)%s%s", sock_err.empty() ? "" : " | ",
				 sock_err.empty() ? "" : sock_err.c_str());
		}
		obs_data_set_string(settings, "stream_fps_debug", fps_buf);

		obs_data_release(settings);
	}

	return props;
}
