// Brother P-touch raster protocol (PT-D600 and its 128-pin, 180 dpi
// siblings). Plain C++ with no Arduino dependency so it builds and tests on
// the host too. Written from Brother's published raster command reference.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <functional>

namespace pt {

constexpr int kHeadPins = 128;   // print head pins across the tape
constexpr int kDpi = 180;        // both directions in normal resolution
constexpr int kLineBytes = kHeadPins / 8;

// 32-byte status block returned for ESC i S and sent unsolicited during a job.
struct Status {
  uint8_t raw[32];
  bool valid = false;

  uint8_t model() const { return raw[4]; }
  uint8_t err1() const { return raw[8]; }
  uint8_t err2() const { return raw[9]; }
  uint8_t width_mm() const { return raw[10]; }
  uint8_t media_type() const { return raw[11]; }
  uint8_t mode() const { return raw[15]; }
  uint8_t length_mm() const { return raw[17]; }
  uint8_t status_type() const { return raw[18]; }
  uint8_t phase() const { return raw[19]; }
  uint16_t phase_number() const { return (uint16_t)(raw[20] << 8 | raw[21]); }
  uint8_t notification() const { return raw[22]; }
  uint8_t tape_color() const { return raw[24]; }
  uint8_t text_color() const { return raw[25]; }
  bool has_error() const { return err1() || err2(); }

  bool parse(const uint8_t* p, size_t n);
};

enum StatusType : uint8_t {
  ST_REPLY = 0x00, ST_PRINT_DONE = 0x01, ST_ERROR = 0x02, ST_EXIT_IF = 0x03,
  ST_TURNED_OFF = 0x04, ST_NOTIFICATION = 0x05, ST_PHASE = 0x06,
};

const char* media_type_name(uint8_t t);
const char* tape_color_name(uint8_t c);
const char* text_color_name(uint8_t c);
const char* model_name(uint8_t m);
const char* status_type_name(uint8_t t);
// Human-readable error text for err1/err2 (empty string when none).
void describe_errors(const Status& s, char* out, size_t n);

// Where the tape sits under the 128-pin head, per tape width.
struct TapeGeom {
  uint8_t width_mm;
  uint8_t left_pins;   // unused pins before the printable area
  uint8_t print_pins;  // printable pins = label image height in dots
};
// nullptr when the width is unknown (no tape, or an unsupported cassette).
const TapeGeom* tape_geom(uint8_t width_mm);

// One label page, read a column at a time. Columns run along the tape in
// reading order (column 0 prints first); each column is `height` dots across
// the tape, packed MSB first into (height+7)/8 bytes, bit set = black, bit 0
// of the column (MSB of byte 0) = top edge of the label as read.
struct Page {
  uint16_t width = 0, height = 0;
  // Fill `out` with column x; false aborts the job (read error).
  std::function<bool(uint32_t x, uint8_t* out)> column;
  static size_t column_bytes(uint16_t h) { return (h + 7) / 8; }
};

// Row-major 1-bit image in RAM (the probe's renderer), adapted to Page.
struct Bitmap {
  uint16_t width = 0, height = 0;
  const uint8_t* bits = nullptr;  // row-major, MSB first, (width+7)/8 bytes a row
  bool get(int x, int y) const {
    return bits[(size_t)y * ((width + 7) / 8) + (x >> 3)] & (0x80 >> (x & 7));
  }
  Page page() const;
};

struct JobOptions {
  bool auto_cut = true;      // cut between labels
  bool chain = false;        // true: don't feed+cut after the last label (saves tape on the next job)
  bool half_cut = false;     // models with a half cutter only
  uint16_t margin_dots = 14; // feed before/after each label; Brother's minimum is 2 mm = 14 dots
  bool mirror = false;
  bool flip_pins = false;    // reverse the pin order (set if the hardware test shows text upside down)
  bool compress = true;      // TIFF PackBits raster lines
};

// Byte sink; returns false to abort the job.
using Sink = std::function<bool(const uint8_t*, size_t)>;

// Writes the whole print job for `n` labels to `out`: invalidate, init, raster
// mode, per-page print info, then raster lines and the page terminators.
bool encode_job(const TapeGeom& tape, uint8_t media_type, const Page* pages, int n,
                const JobOptions& opt, const Sink& out);

// Status request bytes (invalidate + ESC @ + ESC i S).
size_t build_status_request(uint8_t* buf, size_t cap);

// PackBits encode one raster line; returns encoded length (<= len + len/128 + 1).
size_t packbits(const uint8_t* in, size_t len, uint8_t* out);

}  // namespace pt
