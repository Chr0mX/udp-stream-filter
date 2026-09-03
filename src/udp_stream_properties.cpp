// udp_stream_properties.cpp -- OBS Filters-dialog properties: defaults,
// the widget list, and the preset enable/disable modified-callback.
#include "udp_stream_filter.h"

#include <cstdio>

void udp_stream_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "udp_enabled", true);
	obs_data_set_default_string(settings, "target_ip", "127.0.0.1");
	obs_data_set_default_int(settings, "target_port", 5600);
	obs_data_set_default_int(settings, "jpeg_quality", 80);
	obs_data_set_default_int(settings, "max_fps", 0);
	obs_data_set_default_bool(settings, "crop_enabled", false);
	obs_data_set_default_int(settings, "output_preset", 0);
	obs_data_set_default_int(settings, "crop_width", 320);
	obs_data_set_default_int(settings, "crop_height", 320);
	obs_data_set_default_int(settings, "crop_anchor_x", -1);
	obs_data_set_default_int(settings, "crop_anchor_y", -1);
}

// Grays out the manual crop enable/size controls whenever a standard
// output preset (320x320 / 640x640) is active, since their effective
// values are then dictated by the preset rather than user input. The
// anchor stays editable in all cases.
static bool udp_stream_preset_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool is_preset = obs_data_get_int(settings, "output_preset") != 0;

	obs_property_set_enabled(obs_properties_get(props, "crop_enabled"), !is_preset);
	obs_property_set_enabled(obs_properties_get(props, "crop_width"), !is_preset);
	obs_property_set_enabled(obs_properties_get(props, "crop_height"), !is_preset);

	return true;
}

obs_properties_t *udp_stream_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, "udp_enabled", "Enable UDP Streaming");
	obs_properties_add_text(props, "target_ip", "Target IP", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "target_port", "Target Port", 1, 65535, 1);
	obs_properties_add_int_slider(props, "jpeg_quality", "JPEG Quality", 1, 100, 1);
	obs_properties_add_int(props, "max_fps", "Max FPS (0 = uncapped)", 0, 240, 1);
	obs_properties_add_text(props, "stream_fps_debug", "Current Stream FPS (measured)", OBS_TEXT_INFO);

	obs_property_t *preset_list = obs_properties_add_list(props, "output_preset", "Output Preset",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preset_list, "Custom", 0);
	obs_property_list_add_int(preset_list, "320x320", 1);
	obs_property_list_add_int(preset_list, "640x640", 2);
	obs_property_set_modified_callback(preset_list, udp_stream_preset_modified);

	obs_properties_add_bool(props, "crop_enabled", "Enable Crop");
	obs_properties_add_int(props, "crop_width", "Crop Width (px)", 1, 7680, 1);
	obs_properties_add_int(props, "crop_height", "Crop Height (px)", 1, 4320, 1);
	obs_properties_add_int(props, "crop_anchor_x", "Crop Anchor X, px (-1 = center)", -1, 7680, 1);
	obs_properties_add_int(props, "crop_anchor_y", "Crop Anchor Y, px (-1 = center)", -1, 4320, 1);

	if (data) {
		udp_stream_filter *f = (udp_stream_filter *)data;
		obs_data_t *settings = obs_source_get_settings(f->source);
		udp_stream_preset_modified(props, preset_list, settings);

		// Live status text, refreshed each time the properties are
		// (re)built -- see the periodic obs_source_update_properties()
		// call in encode_thread_func() that keeps an open dialog current.
		char fps_buf[128];
		double fps = f->measured_fps.load();
		uint64_t dropped;
		{
			std::lock_guard<std::mutex> lock(f->enc_mtx);
			dropped = f->frames_dropped;
		}
		// Snapshot udp_enabled the same way the render thread does (see
		// udp_stream_filter.h's comment on the settings block) rather than
		// reading f->udp_enabled directly -- purely for consistency with
		// every other reader, since this call already runs on the same
		// settings-update thread context as the writer in practice.
		bool udp_enabled;
		{
			std::lock_guard<std::mutex> net_lock(f->net_mtx);
			udp_enabled = f->udp_enabled;
		}
		if (udp_enabled && fps > 0.0) {
			if (dropped > 0)
				snprintf(fps_buf, sizeof(fps_buf),
					 "%.1f fps  (%llu frame(s) dropped -- encoder behind capture)", fps,
					 (unsigned long long)dropped);
			else
				snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", fps);
		} else {
			snprintf(fps_buf, sizeof(fps_buf), "-- (not streaming)");
		}
		obs_data_set_string(settings, "stream_fps_debug", fps_buf);

		obs_data_release(settings);
	}

	return props;
}
