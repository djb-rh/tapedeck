#include "ptouch.h"
#include <stdio.h>
#include <string.h>

namespace pt {

bool Status::parse(const uint8_t* p, size_t n) {
  valid = false;
  if (n < 32 || p[0] != 0x80 || p[1] != 0x20 || p[2] != 'B') return false;
  memcpy(raw, p, 32);
  valid = true;
  return true;
}

const char* media_type_name(uint8_t t) {
  switch (t) {
    case 0x00: return "No tape";
    case 0x01: return "Laminated";
    case 0x03: return "Non-laminated";
    case 0x04: return "Fabric";
    case 0x11: return "Heat-shrink 2:1";
    case 0x13: return "Fle";
    case 0x14: return "Flexible ID";
    case 0x15: return "Satin";
    case 0x17: return "Heat-shrink 3:1";
    case 0xFF: return "Incompatible";
    default: return "Unknown";
  }
}

const char* tape_color_name(uint8_t c) {
  switch (c) {
    case 0x01: return "White";
    case 0x02: return "Other";
    case 0x03: return "Clear";
    case 0x04: return "Red";
    case 0x05: return "Blue";
    case 0x06: return "Yellow";
    case 0x07: return "Green";
    case 0x08: return "Black";
    case 0x09: return "Clear (white text)";
    case 0x20: return "Matte White";
    case 0x21: return "Matte Clear";
    case 0x22: return "Matte Silver";
    case 0x23: return "Satin Gold";
    case 0x24: return "Satin Silver";
    case 0x30: return "Blue (D)";
    case 0x31: return "Red (D)";
    case 0x40: return "Fluorescent Orange";
    case 0x41: return "Fluorescent Yellow";
    case 0x50: return "Berry Pink (S)";
    case 0x51: return "Light Gray (S)";
    case 0x52: return "Lime Green (S)";
    case 0x60: return "Yellow (F)";
    case 0x61: return "Pink (F)";
    case 0x62: return "Blue (F)";
    case 0x70: return "White (heat-shrink)";
    case 0x90: return "White (flex ID)";
    case 0x91: return "Yellow (flex ID)";
    case 0xF0: return "Cleaning";
    case 0xF1: return "Stencil";
    case 0xFF: return "Incompatible";
    case 0x00: return "None";
    default: return "Unknown";
  }
}

const char* text_color_name(uint8_t c) {
  switch (c) {
    case 0x01: return "White";
    case 0x02: return "Other";
    case 0x04: return "Red";
    case 0x05: return "Blue";
    case 0x08: return "Black";
    case 0x0A: return "Gold";
    case 0x62: return "Blue (F)";
    case 0xF0: return "Cleaning";
    case 0xF1: return "Stencil";
    case 0xFF: return "Incompatible";
    case 0x00: return "None";
    default: return "Unknown";
  }
}

const char* model_name(uint8_t m) {
  switch (m) {
    case 0x68: return "PT-E550W";
    case 0x6A: return "PT-D600";
    case 0x69: return "PT-P750W";
    case 0x6F: return "PT-P900W";
    case 0x70: return "PT-P950NW";
    case 0x78: return "PT-P710BT";
    default: return nullptr;
  }
}

const char* status_type_name(uint8_t t) {
  switch (t) {
    case ST_REPLY: return "reply";
    case ST_PRINT_DONE: return "printing completed";
    case ST_ERROR: return "error";
    case ST_EXIT_IF: return "exit IF mode";
    case ST_TURNED_OFF: return "turned off";
    case ST_NOTIFICATION: return "notification";
    case ST_PHASE: return "phase change";
    default: return "?";
  }
}

const ColorPreset kColorPresets[] = {
  {0x01, 0x08, "Black on White"},   {0x06, 0x08, "Black on Yellow"}, {0x03, 0x08, "Black on Clear"},
  {0x04, 0x08, "Black on Red"},     {0x05, 0x08, "Black on Blue"},   {0x07, 0x08, "Black on Green"},
  {0x22, 0x08, "Black on Silver"},  {0x40, 0x08, "Black on Orange"}, {0x41, 0x08, "Black on Fl. Yellow"},
  {0x01, 0x04, "Red on White"},     {0x01, 0x05, "Blue on White"},   {0x08, 0x01, "White on Black"},
  {0x05, 0x01, "White on Blue"},    {0x04, 0x01, "White on Red"},    {0x07, 0x01, "White on Green"},
  {0x03, 0x01, "White on Clear"},   {0x08, 0x0A, "Gold on Black"},   {0x03, 0x0A, "Gold on Clear"},
};
const int kColorPresetCount = sizeof kColorPresets / sizeof kColorPresets[0];

int find_color_preset(uint8_t tape, uint8_t ink) {
  for (int i = 0; i < kColorPresetCount; ++i)
    if (kColorPresets[i].tape == tape && kColorPresets[i].ink == ink) return i;
  return -1;
}

void describe_errors(const Status& s, char* out, size_t n) {
  static const char* e1[8] = {"No tape", "End of tape", "Cutter jam", "Weak battery",
                              "Printer in use", nullptr, "High-voltage adapter", nullptr};
  static const char* e2[8] = {"Replace tape", "Expansion buffer full", "Transmission error",
                              "Buffer full", "Cover open", "Overheating",
                              "Black mark not detected", "System error"};
  out[0] = 0;
  size_t o = 0;
  auto add = [&](const char* t) {
    if (!t) return;
    int w = snprintf(out + o, n - o, "%s%s", o ? ", " : "", t);
    if (w > 0) o += (size_t)w < n - o ? (size_t)w : n - o - 1;
  };
  for (int b = 0; b < 8; ++b) if (s.err1() & (1 << b)) add(e1[b] ? e1[b] : "Error 1");
  for (int b = 0; b < 8; ++b) if (s.err2() & (1 << b)) add(e2[b]);
}

static const TapeGeom kGeom[] = {
  {4, 52, 24},   // 3.5 mm reports 4
  {6, 48, 32},
  {9, 39, 50},
  {12, 29, 70},
  {18, 8, 112},
  {24, 0, 128},
};

const TapeGeom* tape_geom(uint8_t width_mm) {
  for (const auto& g : kGeom) if (g.width_mm == width_mm) return &g;
  return nullptr;
}

size_t build_status_request(uint8_t* buf, size_t cap) {
  if (cap < 105) return 0;
  memset(buf, 0, 100);
  buf[100] = 0x1B; buf[101] = '@';
  buf[102] = 0x1B; buf[103] = 'i'; buf[104] = 'S';
  return 105;
}

size_t packbits(const uint8_t* in, size_t len, uint8_t* out) {
  size_t i = 0, o = 0;
  while (i < len) {
    // run of identical bytes?
    size_t run = 1;
    while (i + run < len && run < 128 && in[i + run] == in[i]) ++run;
    if (run >= 2) {
      out[o++] = (uint8_t)(257 - run);  // -(run-1)
      out[o++] = in[i];
      i += run;
      continue;
    }
    // literal stretch until the next run of 2+
    size_t lit = 1;
    while (i + lit < len && lit < 128) {
      if (i + lit + 1 < len && in[i + lit] == in[i + lit + 1]) break;
      ++lit;
    }
    out[o++] = (uint8_t)(lit - 1);
    memcpy(out + o, in + i, lit);
    o += lit; i += lit;
  }
  return o;
}

Page Bitmap::page() const {
  Page p;
  p.width = width; p.height = height;
  Bitmap self = *this;
  p.column = [self](uint32_t x, uint8_t* out) {
    memset(out, 0, Page::column_bytes(self.height));
    for (int y = 0; y < self.height; ++y)
      if (self.get((int)x, y)) out[y >> 3] |= 0x80 >> (y & 7);
    return true;
  };
  return p;
}

bool encode_job(const TapeGeom& tape, uint8_t media_type, const Page* pages, int n,
                const JobOptions& opt, const Sink& out) {
  if (n <= 0) return false;
  uint8_t b[160];
  auto put = [&](const uint8_t* p, size_t len) { return out(p, len); };

  // invalidate + initialize + raster mode
  memset(b, 0, 100);
  if (!put(b, 100)) return false;
  const uint8_t init[] = {0x1B, '@'};
  if (!put(init, sizeof init)) return false;
  const uint8_t raster[] = {0x1B, 'i', 'a', 0x01};
  if (!put(raster, sizeof raster)) return false;

  for (int p = 0; p < n; ++p) {
    const Page& bm = pages[p];
    const uint32_t lines = bm.width;
    // ESC i z: print information
    uint8_t pi[13] = {0x1B, 'i', 'z',
                      (uint8_t)(0x80 | 0x04 | (media_type ? 0x02 : 0)),  // recover | width | kind
                      media_type, tape.width_mm, 0,
                      (uint8_t)lines, (uint8_t)(lines >> 8), (uint8_t)(lines >> 16), (uint8_t)(lines >> 24),
                      (uint8_t)(p == 0 ? 0 : 1), 0};
    if (!put(pi, sizeof pi)) return false;
    if (p == 0) {
      // ESC i M: various mode (bit6 auto cut, bit7 mirror)
      uint8_t vm[] = {0x1B, 'i', 'M', (uint8_t)((opt.auto_cut ? 0x40 : 0) | (opt.mirror ? 0x80 : 0))};
      if (!put(vm, sizeof vm)) return false;
      // ESC i K: advanced mode (bit2 half cut, bit3 no chain printing)
      uint8_t am[] = {0x1B, 'i', 'K', (uint8_t)((opt.half_cut ? 0x04 : 0) | (opt.chain ? 0 : 0x08))};
      if (!put(am, sizeof am)) return false;
    }
    // ESC i d: margin (feed) amount, every page
    uint8_t mg[] = {0x1B, 'i', 'd', (uint8_t)opt.margin_dots, (uint8_t)(opt.margin_dots >> 8)};
    if (!put(mg, sizeof mg)) return false;
    // M: compression mode
    uint8_t cm[] = {'M', (uint8_t)(opt.compress ? 0x02 : 0x00)};
    if (!put(cm, sizeof cm)) return false;

    // Raster lines: one per column of the label, pins = rows.
    const int rows = bm.height < tape.print_pins ? bm.height : tape.print_pins;
    const int yoff = (tape.print_pins - rows) / 2;  // centre a shorter image
    uint8_t col[kHeadPins / 8 + 32];
    const size_t cb = Page::column_bytes(bm.height);
    if (cb > sizeof col) return false;
    for (uint32_t x = 0; x < lines; ++x) {
      uint8_t line[kLineBytes] = {0};
      bool any = false;
      if (!bm.column(opt.mirror ? lines - 1 - x : x, col)) return false;
      for (int y = 0; y < rows; ++y) {
        if (!(col[y >> 3] & (0x80 >> (y & 7)))) continue;
        int pin = tape.left_pins + yoff + y;
        if (opt.flip_pins) pin = kHeadPins - 1 - pin;
        line[pin >> 3] |= 0x80 >> (pin & 7);
        any = true;
      }
      if (!any && opt.compress) {
        const uint8_t z = 'Z';
        if (!put(&z, 1)) return false;
        continue;
      }
      if (opt.compress) {
        uint8_t enc[kLineBytes + 4];
        size_t el = packbits(line, kLineBytes, enc);
        b[0] = 'G'; b[1] = (uint8_t)el; b[2] = (uint8_t)(el >> 8);
        memcpy(b + 3, enc, el);
        if (!put(b, 3 + el)) return false;
      } else {
        b[0] = 'g'; b[1] = 0x00; b[2] = kLineBytes;
        memcpy(b + 3, line, kLineBytes);
        if (!put(b, 3 + kLineBytes)) return false;
      }
    }
    const uint8_t term = (p == n - 1) ? 0x1A : 0x0C;  // last page: print with feed; else print
    if (!put(&term, 1)) return false;
  }
  return true;
}

}  // namespace pt
