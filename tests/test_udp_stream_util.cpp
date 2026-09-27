// Standalone unit tests for udp_stream_util.h (no OBS / OpenCV linkage).
#include "../src/udp_stream_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                                 \
	do {                                                                              \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                                     \
		}                                                                         \
	} while (0)

#define EXPECT_EQ(a, b)                                                                                          \
	do {                                                                                                     \
		auto _av = (a);                                                                                  \
		auto _bv = (b);                                                                                  \
		if (_av != _bv) {                                                                                \
			std::fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a,     \
				     (long long)_av, #b, (long long)_bv);                                        \
			g_failures++;                                                                            \
		}                                                                                                \
	} while (0)

static void test_capture_rect_full_frame()
{
	CaptureRect r = compute_capture_rect(1920, 1080, false, 320, 320, -1, -1);
	EXPECT_EQ(r.x, 0);
	EXPECT_EQ(r.y, 0);
	EXPECT_EQ(r.width, 1920);
	EXPECT_EQ(r.height, 1080);
}

static void test_capture_rect_centered_crop()
{
	CaptureRect r = compute_capture_rect(1920, 1080, true, 320, 320, -1, -1);
	EXPECT_EQ(r.width, 320);
	EXPECT_EQ(r.height, 320);
	EXPECT_EQ(r.x, (1920 - 320) / 2);
	EXPECT_EQ(r.y, (1080 - 320) / 2);
}

static void test_capture_rect_clamps_overflow()
{
	CaptureRect r = compute_capture_rect(100, 100, true, 200, 200, 50, 50);
	EXPECT_EQ(r.width, 100);
	EXPECT_EQ(r.height, 100);
	EXPECT_EQ(r.x, 0);
	EXPECT_EQ(r.y, 0);
}

static void test_capture_rect_explicit_anchor()
{
	CaptureRect r = compute_capture_rect(1920, 1080, true, 100, 80, 10, 20);
	EXPECT_EQ(r.x, 10);
	EXPECT_EQ(r.y, 20);
	EXPECT_EQ(r.width, 100);
	EXPECT_EQ(r.height, 80);
}

static void test_preset_sizes()
{
	EXPECT_EQ(preset_crop_size(0), 0);
	EXPECT_EQ(preset_crop_size(1), 160);
	EXPECT_EQ(preset_crop_size(2), 320);
	EXPECT_EQ(preset_crop_size(3), 416);
	EXPECT_EQ(preset_crop_size(4), 512);
	EXPECT_EQ(preset_crop_size(5), 640);
}

static void test_clamp_payload()
{
	EXPECT_EQ((int)clamp_udp_payload(100), 512);
	EXPECT_EQ((int)clamp_udp_payload(1400), 1400);
	EXPECT_EQ((int)clamp_udp_payload(999999), (int)UDP_PAYLOAD_HARD_MAX);
}

static void test_pack_header_big_endian()
{
	uint8_t h[UDP_HEADER_SIZE];
	pack_udp_header(h, 0x01020304u, 0x0a0b0c0du, 0x1122u, 0x3344u, 0x5566u);
	EXPECT_EQ(h[0], 0x01);
	EXPECT_EQ(h[1], 0x02);
	EXPECT_EQ(h[2], 0x03);
	EXPECT_EQ(h[3], 0x04);
	EXPECT_EQ(h[4], 0x0a);
	EXPECT_EQ(h[5], 0x0b);
	EXPECT_EQ(h[6], 0x0c);
	EXPECT_EQ(h[7], 0x0d);
	EXPECT_EQ(h[8], 0x11);
	EXPECT_EQ(h[9], 0x22);
	EXPECT_EQ(h[10], 0x33);
	EXPECT_EQ(h[11], 0x44);
	EXPECT_EQ(h[12], 0x55);
	EXPECT_EQ(h[13], 0x66);
}

static void test_trailer_roundtrip()
{
	std::vector<uint8_t> buf;
	// Fake JPEG ending with EOI
	buf.push_back(0xff);
	buf.push_back(0xd9);
	const uint64_t ts = 0x1122334455667788ULL;
	append_xudp_trailer(buf, ts);
	EXPECT_EQ(buf.size(), 2u + UDP_TRAILER_SIZE);

	uint8_t ver = 0;
	uint64_t out_ts = 0;
	EXPECT_TRUE(parse_xudp_trailer(buf.data(), buf.size(), &ver, &out_ts));
	EXPECT_EQ(ver, UDP_TRAILER_VERSION);
	EXPECT_EQ(out_ts, ts);
	EXPECT_TRUE(buf[0] == 0xff && buf[1] == 0xd9);
}

int main()
{
	test_capture_rect_full_frame();
	test_capture_rect_centered_crop();
	test_capture_rect_clamps_overflow();
	test_capture_rect_explicit_anchor();
	test_preset_sizes();
	test_clamp_payload();
	test_pack_header_big_endian();
	test_trailer_roundtrip();

	if (g_failures) {
		std::fprintf(stderr, "%d test(s) failed\n", g_failures);
		return 1;
	}
	std::printf("All udp_stream_util tests passed\n");
	return 0;
}
