// TapeDeck job file ("TDJ1"): what the web editor uploads, what the API's
// text renderer writes, and what the history keeps for reprints.
//
//   offset size
//   0      4    magic "TDJ1"
//   4      1    flags: bit0 auto cut, bit1 chain (hold last label), bit2 mirror
//   5      1    tape width the job was laid out for, mm (0 = any)
//   6      2    margin (feed) dots, LE
//   8      2    copies of the whole set, LE (1..99)
//   10     2    page count, LE
//   12     4    created, unix seconds LE (0 = unknown; the device fills it in)
//   16     48   title, UTF-8, NUL padded
//   64     ...  pages, each: u16 width (columns), u16 height (dots), then
//               width * ((height+7)/8) bytes, column-major, MSB = top
//
// All integers little-endian. Column-major so the printer can be fed straight
// from flash a column at a time.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tdj {

constexpr size_t kHeaderSize = 64;
constexpr size_t kTitleLen = 48;
constexpr uint16_t kMaxPages = 200;
constexpr uint16_t kMaxWidth = 18000;  // 2.5 m at 180 dpi
constexpr uint16_t kMaxHeight = 128;

enum Flags : uint8_t { F_AUTOCUT = 1, F_CHAIN = 2, F_MIRROR = 4 };

struct Header {
  uint8_t flags = F_AUTOCUT;
  uint8_t tape_mm = 0;
  uint16_t margin_dots = 14;
  uint16_t copies = 1;
  uint16_t pages = 0;
  uint32_t created = 0;
  char title[kTitleLen + 1] = "";
};

struct PageInfo {
  uint16_t width = 0, height = 0;
  uint32_t offset = 0;  // file offset of the first column byte
  size_t bytes() const { return (size_t)width * ((height + 7) / 8); }
};

void write_header(const Header& h, uint8_t out[kHeaderSize]);
// false on bad magic / out-of-range fields.
bool read_header(const uint8_t in[kHeaderSize], Header& h);
void write_page_header(uint16_t w, uint16_t h, uint8_t out[4]);
bool read_page_header(const uint8_t in[4], PageInfo& p);

}  // namespace tdj
