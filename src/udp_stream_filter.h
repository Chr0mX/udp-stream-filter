#pragma once

#include "udp_stream_util.h"

#include <obs-module.h>
#include <obs-source.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <opencv2/core.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#undef min
#undef max
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int SOCKET;
static const SOCKET INVALID_SOCKET = -1;
static const int SOCKET_ERROR = -1;
#define closesocket(s) close(s)
static int WSAGetLastError()
{
	return errno;
}
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Wire header for each UDP packet (14 bytes, all fields big-endian):
//   frame_id     (4) - increments per source frame
//   total_size   (4) - total payload size across all chunks (JPEG + optional trailer)
//   chunk_index  (2) - 0-based index of this chunk
//   total_chunks (2) - total number of chunks for this frame
//   chunk_size   (2) - payload bytes carried in this packet
//
// PROTOCOL NOTE: the 14-byte header has no version field and must not change
// in place -- Axiom's udp_receiver.py unpacks ">IIHHH". Compatible evolution
// is append-only: an XUDP trailer may follow the JPEG EOI inside the chunked
// payload (see udp_stream_util.h). Receivers that only imdecode the assembled
// bytes keep working because JPEG parsers stop at EOI.

struct udp_stream_filter {
	obs_source_t *source;
	gs_texrender_t *texrender;

	// Double-buffered GPU->CPU readback.
	gs_stagesurf_t *stagesurface[2];
	uint32_t stage_width[2];
	uint32_t stage_height[2];
	bool stage_valid[2];
	int stage_write_idx;

	// Settings -- written only by udp_stream_update() as one net_mtx-guarded
	// group; readers snapshot under the same lock.
	bool udp_enabled{false};
	std::string target_ip;
	int target_port{5600};
	int jpeg_quality{80};
	int max_fps{120};
	bool crop_enabled{false};
	int output_preset{0};
	int crop_width{320};
	int crop_height{320};
	int crop_anchor_x{-1};
	int crop_anchor_y{-1};
	// 0 = send native crop pixels; >0 = cv::resize to this square before JPEG.
	int downscale_to{0};
	// Datagram payload bytes (clamped); smaller = safer on Wi-Fi / WAN.
	int udp_payload_size{1400};
	bool show_crop_overlay{true};
	bool append_timestamp_trailer{true};

	// Guards sock/addr and every settings field above.
	std::mutex net_mtx;
	SOCKET sock;
	sockaddr_storage addr;
	socklen_t addr_len;
	std::string last_socket_error;
	// Last IP/port that setup_socket_locked() actually applied -- typing in
	// the Target Host field updates target_ip every keystroke but does not
	// recreate the socket until Apply (or until streaming is toggled on).
	std::string applied_ip;
	int applied_port;

	std::chrono::steady_clock::time_point last_send;
	int frames_sent;
	bool first_sent;

	std::thread enc_thread;
	std::atomic<bool> enc_running{false};
	std::mutex enc_mtx;
	std::condition_variable enc_cv;
	// Two-deep latest-biased queue: capture can leave one frame waiting
	// while encode works on another. When both slots are full the oldest
	// waiting frame is dropped.
	std::array<cv::Mat, 2> pending_q;
	int pending_count{0};
	uint64_t frames_dropped{0};

	std::atomic<double> measured_fps{0.0};
	std::atomic<double> measured_kbps{0.0};
	std::chrono::steady_clock::time_point fps_window_start;
	int fps_window_count = 0;
	uint64_t bytes_window = 0;
	std::chrono::steady_clock::time_point last_status_log;

	// Last-send diagnostics (encode/net thread writes; UI reads under enc_mtx
	// for the counter fields and under net_mtx for last_socket_error).
	uint32_t last_jpeg_size{0};
	uint16_t last_chunk_count{0};
	uint64_t send_errors{0};
};

// --- udp_stream_net.cpp ---
bool setup_socket_locked(udp_stream_filter *f);
void send_jpeg_chunked(udp_stream_filter *f, const uint8_t *jpeg, unsigned long jpeg_size, uint32_t frame_id);

// --- udp_stream_capture.cpp ---
void capture_and_queue_frame(udp_stream_filter *f, obs_source_t *target, uint32_t src_width, uint32_t src_height);
void encode_thread_func(udp_stream_filter *f);

// --- udp_stream_properties.cpp ---
void udp_stream_get_defaults(obs_data_t *settings);
obs_properties_t *udp_stream_get_properties(void *data);

// --- udp_stream_filter.cpp helpers used by render ---
void draw_crop_overlay(float x, float y, float w, float h, float src_w, float src_h);
