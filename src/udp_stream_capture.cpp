// udp_stream_capture.cpp -- GPU-side crop/capture (runs on OBS's render
// thread) and the background JPEG encode thread it feeds.
#include "udp_stream_filter.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

void capture_and_queue_frame(udp_stream_filter *f, obs_source_t *target, uint32_t src_width, uint32_t src_height)
{
	bool crop_enabled;
	int crop_width, crop_height, crop_anchor_x, crop_anchor_y;
	int downscale_to;
	{
		std::lock_guard<std::mutex> net_lock(f->net_mtx);
		crop_enabled = f->crop_enabled;
		crop_width = f->crop_width;
		crop_height = f->crop_height;
		crop_anchor_x = f->crop_anchor_x;
		crop_anchor_y = f->crop_anchor_y;
		downscale_to = f->downscale_to;
	}

	CaptureRect capture_rect = compute_capture_rect((int)src_width, (int)src_height, crop_enabled, crop_width,
							crop_height, crop_anchor_x, crop_anchor_y);
	uint32_t width = (uint32_t)capture_rect.width;
	uint32_t height = (uint32_t)capture_rect.height;

	gs_texrender_reset(f->texrender);

	if (!gs_texrender_begin(f->texrender, width, height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
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

	gs_stage_texture(f->stagesurface[write_idx], tex);
	f->stage_valid[write_idx] = true;

	if (f->stage_valid[read_idx] && f->stage_width[read_idx] == width && f->stage_height[read_idx] == height) {
		uint8_t *mapped_data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(f->stagesurface[read_idx], &mapped_data, &linesize)) {
			cv::Mat bgra((int)height, (int)width, CV_8UC4, mapped_data, linesize);
			cv::Mat bgr;
			cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);

			gs_stagesurface_unmap(f->stagesurface[read_idx]);

			if (downscale_to > 0 && (bgr.cols != downscale_to || bgr.rows != downscale_to)) {
				cv::Mat scaled;
				cv::resize(bgr, scaled, cv::Size(downscale_to, downscale_to), 0, 0, cv::INTER_AREA);
				bgr = std::move(scaled);
			}

			{
				std::lock_guard<std::mutex> lock(f->enc_mtx);
				if (f->pending_count == 2) {
					// Drop the oldest waiting frame; keep the one
					// already mid-queue and the brand-new frame.
					f->pending_q[0] = std::move(f->pending_q[1]);
					f->pending_count = 1;
					f->frames_dropped++;
				}
				f->pending_q[f->pending_count] = std::move(bgr);
				f->pending_count++;
			}
			f->enc_cv.notify_one();
		}
	}

	f->stage_write_idx = read_idx;
}

void encode_thread_func(udp_stream_filter *f)
{
#ifdef _WIN32
	// Keep the encode/send path responsive when the machine is busy with
	// GPU/game load -- mirrors the receiver's elevated recv thread.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

	f->last_status_log = std::chrono::steady_clock::now();

	while (f->enc_running) {
		cv::Mat frame;
		uint32_t frame_id;

		{
			std::unique_lock<std::mutex> lock(f->enc_mtx);
			f->enc_cv.wait(lock, [f] { return f->pending_count > 0 || !f->enc_running; });

			if (!f->enc_running)
				break;

			frame = std::move(f->pending_q[0]);
			if (f->pending_count == 2)
				f->pending_q[0] = std::move(f->pending_q[1]);
			f->pending_count--;
			frame_id = (uint32_t)f->frames_sent;
		}

		if (frame.empty())
			continue;

		// Skip the (comparatively expensive) JPEG encode when there is
		// no usable socket -- e.g. bad host, streaming toggled off
		// mid-flight, or Apply not yet clicked after a host change.
		bool sock_ok;
		int quality;
		bool append_trailer;
		{
			std::lock_guard<std::mutex> net_lock(f->net_mtx);
			sock_ok = f->sock != INVALID_SOCKET && f->udp_enabled;
			quality = f->jpeg_quality;
			append_trailer = f->append_timestamp_trailer;
		}
		if (!sock_ok)
			continue;

		std::vector<uchar> jpeg_buf;
		// Prefer the fast baseline path: no progressive scans, no
		// Huffman optimize pass. OpenCV/vcpkg JPEG on Windows is
		// typically libjpeg-turbo already.
		std::vector<int> encode_params = {
			cv::IMWRITE_JPEG_QUALITY,
			quality,
			cv::IMWRITE_JPEG_OPTIMIZE,
			0,
			cv::IMWRITE_JPEG_PROGRESSIVE,
			0,
		};

		if (!cv::imencode(".jpg", frame, jpeg_buf, encode_params) || jpeg_buf.empty()) {
			blog(LOG_WARNING, "[xudp] JPEG compression failed");
			continue;
		}

		if (append_trailer) {
			using namespace std::chrono;
			uint64_t send_ms =
				(uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
			append_xudp_trailer(jpeg_buf, send_ms);
		}

		send_jpeg_chunked(f, jpeg_buf.data(), (unsigned long)jpeg_buf.size(), frame_id);
		f->frames_sent++;

		// Recompute measured send FPS / bitrate about once per second.
		// Do NOT call obs_source_update_properties() from here -- that
		// rebuilds the Filters dialog and steals focus from open fields.
		f->fps_window_count++;
		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration<double>(now - f->fps_window_start).count();
		if (elapsed >= 1.0) {
			f->measured_fps = f->fps_window_count / elapsed;
			uint64_t bytes;
			{
				std::lock_guard<std::mutex> lock(f->enc_mtx);
				bytes = f->bytes_window;
				f->bytes_window = 0;
			}
			f->measured_kbps = (bytes * 8.0 / elapsed) / 1000.0;
			f->fps_window_count = 0;
			f->fps_window_start = now;
		}

		// Periodic status line in the OBS log -- observability without
		// rebuilding the properties UI.
		double since_log = std::chrono::duration<double>(now - f->last_status_log).count();
		if (since_log >= 5.0) {
			f->last_status_log = now;
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
			blog(LOG_INFO,
			     "[xudp] status: %.1f fps, %.1f kbps, dropped=%llu, send_errors=%llu, "
			     "last_jpeg=%u bytes (%u chunks)",
			     f->measured_fps.load(), f->measured_kbps.load(), (unsigned long long)dropped,
			     (unsigned long long)send_errors, last_jpeg, (unsigned)last_chunks);
		}
	}
}
