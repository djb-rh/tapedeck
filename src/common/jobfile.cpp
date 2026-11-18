#include "jobfile.h"
#include <string.h>

namespace tdj {

static void w16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint16_t r16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t r32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void write_header(const Header& h, uint8_t out[kHeaderSize]) {
  memset(out, 0, kHeaderSize);
  memcpy(out, "TDJ1", 4);
  out[4] = h.flags;
  out[5] = h.tape_mm;
  w16(out + 6, h.margin_dots);
  w16(out + 8, h.copies);
  w16(out + 10, h.pages);
  w32(out + 12, h.created);
  strncpy((char*)out + 16, h.title, kTitleLen);
}

bool read_header(const uint8_t in[kHeaderSize], Header& h) {
  if (memcmp(in, "TDJ1", 4) != 0) return false;
  h.flags = in[4];
  h.tape_mm = in[5];
  h.margin_dots = r16(in + 6);
  h.copies = r16(in + 8);
  h.pages = r16(in + 10);
  h.created = r32(in + 12);
  memcpy(h.title, in + 16, kTitleLen);
  h.title[kTitleLen] = 0;
  if (h.copies < 1 || h.copies > 99) return false;
  if (h.pages < 1 || h.pages > kMaxPages) return false;
  if (h.margin_dots > 720) return false;
  return true;
}

void write_page_header(uint16_t w, uint16_t h, uint8_t out[4]) { w16(out, w); w16(out + 2, h); }

bool read_page_header(const uint8_t in[4], PageInfo& p) {
  p.width = r16(in);
  p.height = r16(in + 2);
  return p.width >= 1 && p.width <= kMaxWidth && p.height >= 1 && p.height <= kMaxHeight;
}

}  // namespace tdj
