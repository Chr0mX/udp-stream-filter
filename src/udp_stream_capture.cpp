// udp_stream_capture.cpp -- GPU-side crop/capture (runs on OBS's render
// thread) and the background JPEG encode thread it feeds.
#include "udp_stream_filter.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <utility> // std::move

// ---------------------------------------------------------------------------
// Native pixel crop (no resize/interpolation)
// ---------------------------------------------------------------------------

// Computes the source-space capture rectangle: crop_width x crop_height
// anchored at (crop_anchor_x, crop_anchor_y) when cropping is enabled (or
// centered when either anchor coordinate is negative, the default), or the
// full source frame otherwise. The size is clamped to the source frame's
// dimensions rather than erroring or padding.
//
// Takes the crop settings as plain parameters rather than reading them off
// `udp_stream_filter` directly: capture_and_queue_frame() below snapshots
// them from the struct under f->net_mtx first (see that function's own
// comment) so this function -- called on OBS's render thread -- never
// touches the struct's settings fields without the lock that guards
// concurrent writes from udp_stream_update(). This also happens to make the
// function trivially unit-testable with no OBS/struct dependency at all.
//
// This is used to size the GPU texrender/stagesurface for capture itself,
// so capture only ever reads back the pixels actually needed -- not the
// full source frame -- which is what makes high FPS (e.g. 120fps) with a
// small crop achievable: the GPU->CPU readback (gs_stagesurface_map) and
// color conversion cost scale with the crop size, not the source size.
static cv::Rect compute_capture_rect(int src_w, int src_h, bool crop_enabled, int crop_width, int crop_height,
				     int crop_anchor_x, int crop_anchor_y)
{
	if (!crop_enabled)
		return cv::Rect(0, 0, src_w, src_h);

	int crop_w = std::max(1, std::min(crop_width, src_w));
	int crop_h = std::max(1, std::min(crop_height, src_h));

	int left = crop_anchor_x >= 0 ? crop_anchor_x : (src_w - crop_w) / 2;
	int top = crop_anchor_y >= 0 ? crop_anchor_y : (src_h - crop_h) / 2;

	// Clamp so the capture rect stays fully inside the source frame.
	left = std::max(0, std::min(left, src_w - crop_w));
	top = std::max(0, std::min(top, src_h - crop_h));

	return cv::Rect(left, top, crop_w, crop_h);
}

void capture_and_queue_frame(udp_stream_filter *f, obs_source_t *target, uint32_t src_width, uint32_t src_height)
{
	// Snapshot the crop settings as one atomic group under net_mtx
	// instead of reading f->crop_enabled/crop_width/crop_height/
	// crop_anchor_x/crop_anchor_y directly -- those five fields are
	// written together by udp_stream_update() under the same lock (see
	// udp_stream_filter.h), and reading them unlocked here let a render
	// call land mid-update and see a torn mix of old and new values
	// (e.g. the new crop_enabled paired with the still-old crop_width/
	// crop_height) for one frame.
	bool crop_enabled;
	int crop_width, crop_height, crop_anchor_x, crop_anchor_y;
	{
		std::lock_guard<std::mutex> net_lock(f->net_mtx);
		crop_enabled = f->crop_enabled;
		crop_width = f->crop_width;
		crop_height = f->crop_height;
		crop_anchor_x = f->crop_anchor_x;
		crop_anchor_y = f->crop_anchor_y;
	}

	cv::Rect capture_rect = compute_capture_rect((int)src_width, (int)src_height, crop_enabled, crop_width,
						     crop_height, crop_anchor_x, crop_anchor_y);
	uint32_t width = (uint32_t)capture_rect.width;
	uint32_t height = (uint32_t)capture_rect.height;

	gs_texrender_reset(f->texrender);

	if (!gs_texrender_begin(f->texrender, width, height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	// Ortho bounds are the capture rect in source space, not [0,width]x[0,height]:
	// mapping just that sub-rect onto the full (width x height) viewport captures
	// exactly those source pixels at 1:1 scale -- a GPU-side crop with no resize,
	// and critically, no readback of the full source frame when cropping is on.
	gs_ortho((float)capture_rect.x, (float)(capture_rect.x + capture_rect.width), (float)capture_rect.y,
		 (float)(capture_rect.y + capture_rect.height), -100.0f, 100.0f);

	obs_source_video_render(target);

	gs_texrender_end(f->texrender);

	gs_texture_t *tex = gs_texrender_get_texture(f->texrender);
	if (!tex)
		return;

	int write_idx = f->stage_write_idx;
	int read_idx = 1 - write_idx;

	if (!f->stagesurface[write_idx] || f->stage_width[write_idx] != width || f->stage_height[write_idx] != height) {
		if (f->stagesurface[write_idx])
			gs_stagesurface_destroy(f->stagesurface[write_idx]);
		f->stagesurface[write_idx] = gs_stagesurface_create(width, height, GS_BGRA);
		f->stage_width[write_idx] = width;
		f->stage_height[write_idx] = height;
		f->stage_valid[write_idx] = false;
	}

	// Queue this frame's GPU->CPU copy; it completes asynchronously and is
	// only mapped on a *later* call (see below), so this never blocks.
	gs_stage_texture(f->stagesurface[write_idx], tex);
	f->stage_valid[write_idx] = true;

	// Read back the other buffer's copy, staged on a previous call -- by
	// now the GPU has had at least one full frame to finish it, so this
	// map should return immediately instead of stalling.
	if (f->stage_valid[read_idx] && f->stage_width[read_idx] == width && f->stage_height[read_idx] == height) {
		uint8_t *mapped_data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(f->stagesurface[read_idx], &mapped_data, &linesize)) {
			cv::Mat bgra((int)height, (int)width, CV_8UC4, mapped_data, linesize);
			cv::Mat bgr;
			cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);

			gs_stagesurface_unmap(f->stagesurface[read_idx]);

			// No clone here. cvtColor allocated `bgr` itself, so it
			// already owns its pixels and does not alias mapped_data --
			// the clone this replaced was copying a buffer that was
			// already private, purely out of caution about the unmap
			// above. Moving into `pending` hands that allocation
			// straight to the encode thread instead.
			{
				std::lock_guard<std::mutex> lock(f->enc_mtx);
				// Overwriting an untaken frame means the encoder
				// couldn't keep up and that frame never reaches the
				// wire. Latest-wins is the right policy for a realtime
				// stream, but it was previously invisible: the only
				// symptom was a measured_fps below Max FPS with nothing
				// to attribute it to. Counting it separates "the
				// encoder is the bottleneck" from "the capture gate is
				// pacing us" -- two very different things to act on.
				if (f->has_pending)
					f->frames_dropped++;
				f->pending = std::move(bgr);
				f->has_pending = true;
			}
			f->enc_cv.notify_one();
		}
	}

	f->stage_write_idx = read_idx;
}

// ---------------------------------------------------------------------------
// Background JPEG encode thread
// ---------------------------------------------------------------------------

void encode_thread_func(udp_stream_filter *f)
{
	while (f->enc_running) {
		cv::Mat frame;
		uint32_t frame_id;

		{
			std::unique_lock<std::mutex> lock(f->enc_mtx);
			f->enc_cv.wait(lock, [f] { return f->has_pending || !f->enc_running; });

			if (!f->enc_running)
				break;

			// Move, don't clone. `pending` is dropped either way -
			// has_pending goes false immediately below, and the capture
			// thread assigns a whole new Mat next frame rather than
			// writing into this one - so there is nothing left to
			// protect by copying. Moving hands the allocation over and
			// leaves `pending` empty, which the has_pending flag
			// already represents.
			frame = std::move(f->pending);
			f->has_pending = false;
			frame_id = (uint32_t)f->frames_sent;
		}

		if (frame.empty())
			continue;

		// Snapshot under the lock - jpeg_quality is written by the UI
		// thread in udp_stream_update(). Copied out rather than held so
		// the (comparatively slow) imencode below runs unlocked.
		int quality;
		{
			std::lock_guard<std::mutex> net_lock(f->net_mtx);
			quality = f->jpeg_quality;
		}

		std::vector<uchar> jpeg_buf;
		std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, quality};

		if (cv::imencode(".jpg", frame, jpeg_buf, encode_params) && !jpeg_buf.empty()) {
			send_jpeg_chunked(f, jpeg_buf.data(), (unsigned long)jpeg_buf.size(), frame_id);
			f->frames_sent++;

			// Recompute the measured send FPS about once per second for
			// accuracy, but only nudge the UI to refresh every few
			// seconds -- obs_source_update_properties() rebuilds the
			// whole Filters properties dialog, which fights typing/
			// clicking in other fields (e.g. Max FPS) if done too often.
			f->fps_window_count++;
			auto now = std::chrono::steady_clock::now();
			double elapsed = std::chrono::duration<double>(now - f->fps_window_start).count();
			if (elapsed >= 1.0) {
				f->measured_fps = f->fps_window_count / elapsed;
				f->fps_window_count = 0;
				f->fps_window_start = now;

				double since_ui_refresh =
					std::chrono::duration<double>(now - f->last_ui_refresh).count();
				if (since_ui_refresh >= 4.0) {
					f->last_ui_refresh = now;
					obs_source_update_properties(f->source);
				}
			}
		} else {
			blog(LOG_WARNING, "[xudp] JPEG compression failed");
		}
	}
}
