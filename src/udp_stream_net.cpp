// udp_stream_net.cpp -- UDP socket lifecycle and the chunked wire-protocol
// send path. See udp_stream_filter.h for the wire header layout and the
// net_mtx contract these functions rely on.
#include "udp_stream_filter.h"

#include <algorithm>
#include <cstring>

// Caller must hold f->net_mtx.
bool setup_socket_locked(udp_stream_filter *f)
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

void send_jpeg_chunked(udp_stream_filter *f, const uint8_t *jpeg, unsigned long jpeg_size, uint32_t frame_id)
{
	if (jpeg_size == 0)
		return;

	// total_chunks below is a uint16_t, which can only represent up to
	// 65535 chunks. Casting a larger count down to it would silently
	// wrap to a small value, and the send loop below (bounded by that
	// wrapped total_chunks) would stop after sending only that many
	// chunks -- the receiver would see a "complete" frame (every
	// announced chunk arrived) that is actually a truncated, corrupt
	// JPEG, with nothing pointing back at this as the cause. Not
	// realistically reachable by this plugin's own cv::imencode() output
	// for any plausible source resolution/quality (even an uncompressed
	// 8K BGR frame is ~200 MB, compressed JPEG output is orders of
	// magnitude smaller than the ~3.93 GB threshold below), but worth
	// failing loudly instead of silently corrupting the stream if it
	// ever is.
	const unsigned long long max_representable_size = 65535ULL * (unsigned long long)UDP_MAX_PAYLOAD;
	if ((unsigned long long)jpeg_size > max_representable_size) {
		blog(LOG_WARNING,
		     "[xudp] encoded frame too large to send (%lu bytes, max %llu representable "
		     "in a uint16_t chunk count) -- dropping frame",
		     jpeg_size, max_representable_size);
		return;
	}

	// Held across the whole send so the UI thread can't close the socket or
	// rewrite the destination address between chunks of one frame. sendto()
	// on a UDP socket only copies into the socket buffer and returns, so
	// this never blocks the UI thread for meaningfully long - and a settings
	// change waiting out one frame's worth of chunks is the correct
	// trade against sending on a closed descriptor.
	std::lock_guard<std::mutex> net_lock(f->net_mtx);

	if (f->sock == INVALID_SOCKET)
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
		memcpy(&packet[p], &n_frame_id, 4);
		p += 4;
		memcpy(&packet[p], &n_total_size, 4);
		p += 4;
		memcpy(&packet[p], &n_chunk_index, 2);
		p += 2;
		memcpy(&packet[p], &n_total_chunks, 2);
		p += 2;
		memcpy(&packet[p], &n_chunk_size, 2);
		p += 2;
		memcpy(&packet[p], jpeg + offset, this_size);
		p += this_size;

		int sent = sendto(f->sock, (const char *)packet.data(), (int)p, 0, (const sockaddr *)&f->addr,
				  sizeof(f->addr));

		if (sent == SOCKET_ERROR) {
			blog(LOG_WARNING, "[xudp] sendto failed: %d", WSAGetLastError());
			break;
		}

		offset += this_size;
	}
}
