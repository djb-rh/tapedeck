#include <unity.h>
#include <string.h>
#include <vector>
#include "ptouch.h"
#include "jobfile.h"

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> unpack(const uint8_t* p, size_t n) {
  std::vector<uint8_t> o;
  size_t i = 0;
  while (i < n) {
    int8_t c = (int8_t)p[i++];
    if (c >= 0) { for (int k = 0; k <= c; ++k) o.push_back(p[i++]); }
    else if (c != -128) { for (int k = 0; k < 1 - c; ++k) o.push_back(p[i]); ++i; }
  }
  return o;
}

static void test_packbits_roundtrip() {
  uint8_t cases[][16] = {
    {0},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
    {0, 0, 0, 1, 2, 2, 3, 4, 4, 4, 4, 5, 6, 7, 7, 0},
  };
  for (auto& c : cases) {
    uint8_t enc[40];
    size_t n = pt::packbits(c, 16, enc);
    auto d = unpack(enc, n);
    TEST_ASSERT_EQUAL(16, d.size());
    TEST_ASSERT_EQUAL_MEMORY(c, d.data(), 16);
  }
  uint8_t z[16] = {0}, enc[40];
  TEST_ASSERT_EQUAL(2, pt::packbits(z, 16, enc));
}

static void test_status_parse() {
  uint8_t b[32] = {0x80, 0x20, 'B', '0', 0x00, '0'};
  b[10] = 12; b[11] = 0x01; b[24] = 0x01; b[25] = 0x08;
  pt::Status s;
  TEST_ASSERT_TRUE(s.parse(b, 32));
  TEST_ASSERT_EQUAL(12, s.width_mm());
  TEST_ASSERT_EQUAL_STRING("White", pt::tape_color_name(s.tape_color()));
  TEST_ASSERT_EQUAL_STRING("Black", pt::text_color_name(s.text_color()));
  b[0] = 0;
  TEST_ASSERT_FALSE(s.parse(b, 32));
}

static void test_encode_layout() {
  // 3x4 image on 12 mm tape, single black dot at (1, 0)
  uint8_t bits[4] = {0x40, 0, 0, 0};
  pt::Bitmap bm; bm.width = 3; bm.height = 4; bm.bits = bits;
  pt::Page pg = bm.page();
  std::vector<uint8_t> out;
  pt::JobOptions o; o.compress = false;
  bool ok = pt::encode_job(*pt::tape_geom(12), 0x01, &pg, 1, o,
                           [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); return true; });
  TEST_ASSERT_TRUE(ok);
  // 100 zeros, ESC @, ESC i a 1
  TEST_ASSERT_EQUAL(0x1B, out[100]);
  TEST_ASSERT_EQUAL('@', out[101]);
  // count raster lines
  int g = 0; size_t i = 0;
  for (i = 0; i + 2 < out.size(); ++i) if (out[i] == 'g' && out[i + 1] == 0 && out[i + 2] == 16) { ++g; }
  TEST_ASSERT_EQUAL(3, g);
  TEST_ASSERT_EQUAL(0x1A, out.back());
  // second raster line has pin 29 + (70-4)/2 = 62 set
  size_t first = 0;
  for (i = 0; i + 2 < out.size(); ++i) if (out[i] == 'g' && out[i + 1] == 0 && out[i + 2] == 16) { first = i; break; }
  const uint8_t* l1 = &out[first + 19 + 3];
  int pin = 29 + 33;
  TEST_ASSERT_EQUAL_HEX8(0x80 >> (pin & 7), l1[pin >> 3]);
}

static void test_jobfile_header() {
  tdj::Header h;
  h.flags = tdj::F_AUTOCUT | tdj::F_CHAIN; h.tape_mm = 12; h.margin_dots = 14; h.copies = 3; h.pages = 2; h.created = 1763500000;
  strcpy(h.title, "ROM labels");
  uint8_t b[tdj::kHeaderSize];
  tdj::write_header(h, b);
  tdj::Header r;
  TEST_ASSERT_TRUE(tdj::read_header(b, r));
  TEST_ASSERT_EQUAL(3, r.copies);
  TEST_ASSERT_EQUAL(2, r.pages);
  TEST_ASSERT_EQUAL(12, r.tape_mm);
  TEST_ASSERT_EQUAL_STRING("ROM labels", r.title);
  b[0] = 'X';
  TEST_ASSERT_FALSE(tdj::read_header(b, r));
  uint8_t ph[4];
  tdj::write_page_header(300, 70, ph);
  tdj::PageInfo pi;
  TEST_ASSERT_TRUE(tdj::read_page_header(ph, pi));
  TEST_ASSERT_EQUAL(300 * 9, pi.bytes());
  tdj::write_page_header(10, 200, ph);
  TEST_ASSERT_FALSE(tdj::read_page_header(ph, pi));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_packbits_roundtrip);
  RUN_TEST(test_status_parse);
  RUN_TEST(test_encode_layout);
  RUN_TEST(test_jobfile_header);
  return UNITY_END();
}
