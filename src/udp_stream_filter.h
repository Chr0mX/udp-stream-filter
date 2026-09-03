#pragma once

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

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <condition_variable>
#include <vector>
#include <cstdint>

// Wire header for each UDP packet (14 bytes, all fields big-endian):
//   frame_id     (4 bytes) - increments per source frame, lets receiver detect drops/reorders
//   total_size   (4 bytes) - total JPEG size in bytes across all chunks of this frame
//   chunk_index  (2 bytes) - 0-based index of this chunk
//   total_chunks (2 bytes) - total number of chunks for this frame
//   chunk_size   (2 bytes) - payload bytes carried in this packet
//
// 4 + 4 + 2 + 2 + 2 = 14. This constant previously read 16 while the code
// below wrote 14, and the comment above it claimed 16 too -- harmless only
// because it is used solely to size the send buffer (so it over-allocated
// by 2 bytes) and never as a write offset. Axiom's receiver has always
// unpacked 14 (">IIHHH"), so the wire format itself was never in doubt;
// the constant was just wrong about it, which is exactly the sort of thing
// that bites whoever next reaches for it as an offset.
//
// PROTOCOL NOTE: there is no version field in this header, and adding one
// now would break every deployed receiver. Any future change must stay
// backward-compatible by construction -- append-only payload semantics, or
// a new port. See Axiom's udp_receiver.py for the other half of this
// contract; the two must be changed together.
static const size_t UDP_HEADER_SIZE = 14;
static const size_t UDP_MAX_PAYLOAD = 60000; // LAN-only: let IP fragmentation handle MTU,
					     // most frames fit in 1-2 datagrams instead of ~45

struct udp_stream_filter {
	obs_source_t *source;
	gs_texrender_t *texrender;

	// Double-buffered GPU->CPU readback: gs_stage_texture() queues an async
	// GPU copy into the "write" buffer, then the previous frame's "read"
	// buffer (staged last call, and by now finished on the GPU) is mapped
	// -- so gs_stagesurface_map() never has to block waiting for the
	// current frame's copy to complete. Costs one frame of latency but
	// removes the per-frame GPU pipeline stall that was capping FPS well
	// below both the canvas rate and the Max FPS setting.
	gs_stagesurf_t *stagesurface[2];
	uint32_t stage_width[2];
	uint32_t stage_height[2];
	bool stage_valid[2];
	int stage_write_idx;

	// Settings -- written only by udp_stream_update() (the OBS
	// settings-update callback) as one net_mtx-guarded group, and read
	// either by OBS's render thread (udp_stream_video_render() /
	// capture_and_queue_frame(), both of which snapshot what they need
	// under net_mtx before using it -- see those functions) or by the
	// encode thread (jpeg_quality directly; target_ip/target_port
	// indirectly via `addr`, which setup_socket_locked() rebuilds under
	// this same lock).
	//
	// Previously only jpeg_quality (and sock/addr) were consistently
	// guarded this way -- udp_enabled, max_fps, and the five crop_* /
	// output_preset fields were written unlocked even though the render
	// thread reads several of them every frame. That is a data race
	// under the C++ memory model (aligned-scalar tearing isn't
	// observable in practice on x86/x64, but it's still undefined
	// behavior), and the five crop-rect fields specifically must change
	// together as one group -- a render call landing mid-update could
	// otherwise see a torn mix of old and new values (e.g. a freshly
	// enabled crop_enabled paired with the still-old crop_width/
	// crop_height). All settings fields are now written and read as one
	// locked group so a reader can never observe a partially-applied
	// update.
	bool udp_enabled;
	std::string target_ip;
	int target_port;
	int jpeg_quality;
	int max_fps;
	bool crop_enabled;
	int output_preset;
	int crop_width;
	int crop_height;
	int crop_anchor_x;
	int crop_anchor_y;

	// Guards sock/addr and every settings field above -- see the comment
	// on the settings block itself. sock/addr specifically are written
	// by OBS's UI thread in udp_stream_update() (a settings change can
	// close and recreate the socket) and read by the encode thread in
	// send_jpeg_chunked(). Without it, changing Target IP or Port
	// mid-stream could have the encode thread call sendto() on a
	// descriptor the UI thread had just closed, or read a sockaddr while
	// memset()/inet_pton() were half-way through rewriting it. enc_mtx
	// does NOT cover this - it only guards the pending frame handoff.
	std::mutex net_mtx;
	SOCKET sock;
	sockaddr_in addr;
	std::chrono::steady_clock::time_point last_send;
	int frames_sent;
	bool first_sent;

	std::thread enc_thread;
	std::atomic<bool> enc_running{false};
	std::mutex enc_mtx;
	std::condition_variable enc_cv;
	cv::Mat pending;
	bool has_pending{false};
	// Frames the capture thread overwrote before the encode thread took
	// them, i.e. dropped because encoding couldn't keep up. Guarded by
	// enc_mtx (written by the capture thread, read by the UI thread when
	// properties are rebuilt).
	uint64_t frames_dropped{0};

	// Live-measured FPS of frames actually sent over UDP (i.e. what the
	// receiver sees), independent of the Max FPS cap. Updated by the encode
	// thread roughly once per second; read from the UI thread when the
	// filter's properties are (re)built.
	std::atomic<double> measured_fps{0.0};
	std::chrono::steady_clock::time_point fps_window_start;
	int fps_window_count = 0;
	std::chrono::steady_clock::time_point last_ui_refresh;
};

// --- udp_stream_net.cpp ---
// Caller must hold f->net_mtx.
bool setup_socket_locked(udp_stream_filter *f);
// Takes f->net_mtx itself for the whole (possibly multi-datagram) send.
void send_jpeg_chunked(udp_stream_filter *f, const uint8_t *jpeg, unsigned long jpeg_size, uint32_t frame_id);

// --- udp_stream_capture.cpp ---
// Renders+crops one frame on OBS's render thread and hands it to the encode
// thread; snapshots the crop_* settings under f->net_mtx itself.
void capture_and_queue_frame(udp_stream_filter *f, obs_source_t *target, uint32_t src_width, uint32_t src_height);
// Runs on f->enc_thread for the filter's whole lifetime.
void encode_thread_func(udp_stream_filter *f);

// --- udp_stream_properties.cpp ---
void udp_stream_get_defaults(obs_data_t *settings);
obs_properties_t *udp_stream_get_properties(void *data);
