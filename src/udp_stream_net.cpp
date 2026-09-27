// udp_stream_net.cpp -- UDP socket lifecycle and the chunked wire-protocol
// send path. See udp_stream_filter.h for the wire header layout and the
// net_mtx contract these functions rely on.
#include "udp_stream_filter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Caller must hold f->net_mtx.
bool setup_socket_locked(udp_stream_filter *f)
{
	if (f->sock != INVALID_SOCKET) {
		closesocket(f->sock);
		f->sock = INVALID_SOCKET;
	}
	f->addr_len = 0;
	f->last_socket_error.clear();

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", f->target_port);

	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	addrinfo *res = nullptr;
	int err = getaddrinfo(f->target_ip.c_str(), port_str, &hints, &res);
	if (err != 0 || !res) {
		char buf[256];
		snprintf(buf, sizeof(buf), "resolve failed for '%s' (getaddrinfo=%d)", f->target_ip.c_str(), err);
		f->last_socket_error = buf;
		blog(LOG_ERROR, "[xudp] %s", buf);
		return false;
	}

	SOCKET new_sock = INVALID_SOCKET;
	for (addrinfo *ai = res; ai; ai = ai->ai_next) {
		new_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (new_sock == INVALID_SOCKET)
			continue;

		memset(&f->addr, 0, sizeof(f->addr));
		memcpy(&f->addr, ai->ai_addr, ai->ai_addrlen);
		f->addr_len = (socklen_t)ai->ai_addrlen;
		break;
	}
	freeaddrinfo(res);

	if (new_sock == INVALID_SOCKET || f->addr_len == 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "failed to create UDP socket: %d", WSAGetLastError());
		f->last_socket_error = buf;
		blog(LOG_ERROR, "[xudp] %s", buf);
		return false;
	}

	f->sock = new_sock;

	// Larger send buffer so bursts of multi-chunk frames don't get dropped
	// at the socket layer before reaching the wire.
	int sndbuf = 1 * 1024 * 1024;
	setsockopt(f->sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

	f->applied_ip = f->target_ip;
	f->applied_port = f->target_port;
	f->last_socket_error.clear();
	return true;
}

void send_jpeg_chunked(udp_stream_filter *f, const uint8_t *jpeg, unsigned long jpeg_size, uint32_t frame_id)
{
	if (jpeg_size == 0)
		return;

	size_t payload_size;
	{
		std::lock_guard<std::mutex> net_lock(f->net_mtx);
		payload_size = clamp_udp_payload(f->udp_payload_size);
	}

	const unsigned long long max_representable_size = 65535ULL * (unsigned long long)payload_size;
	if ((unsigned long long)jpeg_size > max_representable_size) {
		blog(LOG_WARNING,
		     "[xudp] encoded frame too large to send (%lu bytes, max %llu representable "
		     "in a uint16_t chunk count) -- dropping frame",
		     jpeg_size, max_representable_size);
		std::lock_guard<std::mutex> lock(f->enc_mtx);
		f->send_errors++;
		return;
	}

	// Held across the whole send so the UI thread can't close the socket or
	// rewrite the destination address between chunks of one frame.
	std::lock_guard<std::mutex> net_lock(f->net_mtx);

	if (f->sock == INVALID_SOCKET)
		return;

	uint16_t total_chunks = (uint16_t)((jpeg_size + payload_size - 1) / payload_size);
	if (total_chunks == 0)
		total_chunks = 1;

	std::vector<uint8_t> packet(UDP_HEADER_SIZE + payload_size);
	size_t offset = 0;
	uint64_t bytes_this_frame = 0;
	bool had_error = false;

	for (uint16_t i = 0; i < total_chunks; i++) {
		size_t remaining = jpeg_size - offset;
		size_t this_size = std::min(remaining, payload_size);

		pack_udp_header(packet.data(), frame_id, (uint32_t)jpeg_size, i, total_chunks, (uint16_t)this_size);
		memcpy(packet.data() + UDP_HEADER_SIZE, jpeg + offset, this_size);

		int sent = sendto(f->sock, (const char *)packet.data(), (int)(UDP_HEADER_SIZE + this_size), 0,
				  (const sockaddr *)&f->addr, f->addr_len);

		if (sent == SOCKET_ERROR) {
			int e = WSAGetLastError();
			char buf[128];
			snprintf(buf, sizeof(buf), "sendto failed: %d", e);
			f->last_socket_error = buf;
			blog(LOG_WARNING, "[xudp] %s", buf);
			had_error = true;
			break;
		}

		bytes_this_frame += (uint64_t)sent;
		offset += this_size;
	}

	{
		std::lock_guard<std::mutex> lock(f->enc_mtx);
		f->last_jpeg_size = (uint32_t)jpeg_size;
		f->last_chunk_count = total_chunks;
		f->bytes_window += bytes_this_frame;
		if (had_error)
			f->send_errors++;
	}
}
