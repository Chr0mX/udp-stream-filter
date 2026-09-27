#pragma once

// Pure helpers shared by the filter and the unit-test binary. No OBS types,
// no sockets, no OpenCV -- so tests can link this with nothing but a C++
// toolchain.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

static const size_t UDP_HEADER_SIZE = 14;

// Hard ceiling on a single datagram payload. The UI may offer values up to
// this; anything larger would not fit in a uint16_t chunk_size field.
static const size_t UDP_PAYLOAD_HARD_MAX = 60000;

// Optional trailer appended *after* the JPEG EOI marker. JPEG decoders
// (including OpenCV's imdecode / libjpeg) stop at EOI, so existing receivers
// that ignore trailing bytes keep working. New receivers may parse this.
// Layout (big-endian after the 4-byte magic):
//   magic      (4)  'X''U''D''P'
//   version    (1)  1
//   flags      (1)  reserved, currently 0
//   send_ms    (8)  sender unix-time milliseconds when the frame was encoded
static const size_t UDP_TRAILER_SIZE = 14;
static const uint8_t UDP_TRAILER_VERSION = 1;

struct CaptureRect {
	int x;
	int y;
	int width;
	int height;
};

// Source-space capture rectangle for a crop (or the full frame when cropping
// is off). Size is clamped to the source; negative anchors mean "center".
inline CaptureRect compute_capture_rect(int src_w, int src_h, bool crop_enabled, int crop_width, int crop_height,
					int crop_anchor_x, int crop_anchor_y)
{
	if (src_w < 1)
		src_w = 1;
	if (src_h < 1)
		src_h = 1;

	if (!crop_enabled)
		return CaptureRect{0, 0, src_w, src_h};

	int crop_w = std::max(1, std::min(crop_width, src_w));
	int crop_h = std::max(1, std::min(crop_height, src_h));

	int left = crop_anchor_x >= 0 ? crop_anchor_x : (src_w - crop_w) / 2;
	int top = crop_anchor_y >= 0 ? crop_anchor_y : (src_h - crop_h) / 2;

	left = std::max(0, std::min(left, src_w - crop_w));
	top = std::max(0, std::min(top, src_h - crop_h));

	return CaptureRect{left, top, crop_w, crop_h};
}

// Output-preset id -> forced square crop size. 0 means Custom (no force).
inline int preset_crop_size(int output_preset)
{
	switch (output_preset) {
	case 1:
		return 160;
	case 2:
		return 320;
	case 3:
		return 416;
	case 4:
		return 512;
	case 5:
		return 640;
	default:
		return 0;
	}
}

inline size_t clamp_udp_payload(int requested)
{
	if (requested < 512)
		return 512;
	if ((size_t)requested > UDP_PAYLOAD_HARD_MAX)
		return UDP_PAYLOAD_HARD_MAX;
	return (size_t)requested;
}

// Pack the 14-byte big-endian wire header into `out` (must have room for
// UDP_HEADER_SIZE bytes).
inline void pack_udp_header(uint8_t *out, uint32_t frame_id, uint32_t total_size, uint16_t chunk_index,
			    uint16_t total_chunks, uint16_t chunk_size)
{
	auto put_u32 = [](uint8_t *p, uint32_t v) {
		p[0] = (uint8_t)((v >> 24) & 0xff);
		p[1] = (uint8_t)((v >> 16) & 0xff);
		p[2] = (uint8_t)((v >> 8) & 0xff);
		p[3] = (uint8_t)(v & 0xff);
	};
	auto put_u16 = [](uint8_t *p, uint16_t v) {
		p[0] = (uint8_t)((v >> 8) & 0xff);
		p[1] = (uint8_t)(v & 0xff);
	};
	put_u32(out + 0, frame_id);
	put_u32(out + 4, total_size);
	put_u16(out + 8, chunk_index);
	put_u16(out + 10, total_chunks);
	put_u16(out + 12, chunk_size);
}

// Append the XUDP v1 trailer to `buf` which already holds a complete JPEG.
// `buf` must be a growable byte buffer (vector-like: data/size/insert end).
template<typename ByteVector> inline void append_xudp_trailer(ByteVector &buf, uint64_t send_unix_ms)
{
	uint8_t trailer[UDP_TRAILER_SIZE];
	trailer[0] = 'X';
	trailer[1] = 'U';
	trailer[2] = 'D';
	trailer[3] = 'P';
	trailer[4] = UDP_TRAILER_VERSION;
	trailer[5] = 0; // flags
	for (int i = 0; i < 8; i++)
		trailer[6 + i] = (uint8_t)((send_unix_ms >> (56 - 8 * i)) & 0xff);
	buf.insert(buf.end(), trailer, trailer + UDP_TRAILER_SIZE);
}

// Returns true if `data` ends with a well-formed XUDP trailer; optionally
// reports version and timestamp. Used by unit tests (and available to
// receivers that want to share the same parser).
inline bool parse_xudp_trailer(const uint8_t *data, size_t size, uint8_t *out_version, uint64_t *out_send_ms)
{
	if (size < UDP_TRAILER_SIZE)
		return false;
	const uint8_t *t = data + size - UDP_TRAILER_SIZE;
	if (t[0] != 'X' || t[1] != 'U' || t[2] != 'D' || t[3] != 'P')
		return false;
	if (out_version)
		*out_version = t[4];
	if (out_send_ms) {
		uint64_t ms = 0;
		for (int i = 0; i < 8; i++)
			ms = (ms << 8) | t[6 + i];
		*out_send_ms = ms;
	}
	return true;
}
