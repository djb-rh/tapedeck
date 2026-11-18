// TapeDeck probe: enumerates the P-touch on the USB Module, dumps its
// descriptors and IEEE 1284 device ID, reads its status block, and prints
// test labels. Serial commands (115200):
//   s            status request
//   i            descriptors + device id
//   t TEXT       print one label with TEXT
//   o            orientation test label
//   m N TEXT     print N copies in one job (chain test)
//   c 0|1        chain (hold last label) off/on for following prints
//   f 0|1        flip pins
#include <Arduino.h>
#include <M5Unified.h>
#include "M5_Max3421E_Usb.h"
#include "printer_usb.h"
#include "ptouch.h"

static int g_cs = -1;
static pt::Status g_st;
static pt::JobOptions g_opt;
extern "C" volatile uint32_t tapedeck_out_naks;
static bool g_dry = false;      // x: dump job bytes as hex instead of printing
static int g_init_pause = 0;
static int g_lead = 0;          // l N: blank columns before page 1    // p N: ms pause after invalidate+ESC @

static uint8_t bb_xfer(uint8_t out) {
  uint8_t in = 0;
  for (int b = 7; b >= 0; --b) {
    digitalWrite(23, (out >> b) & 1); delayMicroseconds(2);
    digitalWrite(18, HIGH); delayMicroseconds(2);
    in = (in << 1) | digitalRead(19);
    digitalWrite(18, LOW); delayMicroseconds(2);
  }
  return in;
}

static int find_max3421_cs() {
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  pinMode(23, OUTPUT); digitalWrite(23, LOW);
  pinMode(19, INPUT);
  const int cands[] = {13, 5, 0};
  int found = -1;
  for (int cs : cands) {
    pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); delayMicroseconds(10);
    digitalWrite(cs, LOW); bb_xfer((17 << 3) | 0x02); bb_xfer(0x10); digitalWrite(cs, HIGH);
    delayMicroseconds(10);
    digitalWrite(cs, LOW); bb_xfer(18 << 3); uint8_t rev = bb_xfer(0); digitalWrite(cs, HIGH);
    Serial.printf("[usb] CS G%d: REVISION 0x%02X\r\n", cs, rev);
    if (found < 0 && (rev == 0x12 || rev == 0x13)) found = cs;
  }
  pinMode(18, INPUT); pinMode(23, INPUT);
  return found;
}

static void hexdump(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) Serial.printf("%02X%s", p[i], ((i & 15) == 15 || i + 1 == n) ? "\r\n" : " ");
}

static void lcd(const char* a, const char* b = nullptr, const char* c = nullptr) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 4); M5.Display.println("TapeDeck probe");
  M5.Display.setCursor(4, 50); if (a) M5.Display.println(a);
  M5.Display.setCursor(4, 90); if (b) M5.Display.println(b);
  M5.Display.setCursor(4, 130); if (c) M5.Display.println(c);
}

static void print_status(const pt::Status& s) {
  hexdump(s.raw, 32);
  char err[160];
  pt::describe_errors(s, err, sizeof err);
  const char* mn = pt::model_name(s.model());
  Serial.printf("model 0x%02X%s%s  type %s  phase %u/%u  notif %u\r\n", s.model(), mn ? " " : "", mn ? mn : "",
                pt::status_type_name(s.status_type()), s.phase(), s.phase_number(), s.notification());
  Serial.printf("tape %u mm %s, %s on %s, length %u, mode 0x%02X, err %02X %02X %s\r\n",
                s.width_mm(), pt::media_type_name(s.media_type()), pt::text_color_name(s.text_color()),
                pt::tape_color_name(s.tape_color()), s.length_mm(), s.mode(), s.err1(), s.err2(), err);
  char l1[40], l2[40];
  snprintf(l1, sizeof l1, "%u mm %s", s.width_mm(), pt::media_type_name(s.media_type()));
  snprintf(l2, sizeof l2, "%s on %s", pt::text_color_name(s.text_color()), pt::tape_color_name(s.tape_color()));
  lcd(l1, l2, err[0] ? err : "ready");
}

static bool read_status(pt::Status& s, uint32_t timeout_ms) {
  uint8_t buf[64];
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    int n = printerUsb.read(buf, sizeof buf, timeout_ms - (millis() - t0));
    if (n < 0) return false;
    if (n == 0) continue;
    if (n >= 32 && s.parse(buf, n)) return true;
    Serial.printf("[in] %d bytes: ", n); hexdump(buf, n);
  }
  return false;
}

static bool request_status() {
  uint8_t req[110];
  size_t n = pt::build_status_request(req, sizeof req);
  if (!printerUsb.write(req, n)) { Serial.println("status: write failed"); return false; }
  if (!read_status(g_st, 2000)) { Serial.println("status: no reply"); return false; }
  print_status(g_st);
  return true;
}

static void dump_info() {
  Serial.printf("VID %04X PID %04X  '%s' '%s' '%s'\r\n", printerUsb.vid(), printerUsb.pid(),
                printerUsb.manufacturer(), printerUsb.product(), printerUsb.serial());
  Serial.printf("config descriptor %u bytes:\r\n", printerUsb.config_len());
  hexdump(printerUsb.config_desc(), printerUsb.config_len());
  // IEEE 1284 device ID: class request GET_DEVICE_ID (0xA1, 0, wValue 0, wIndex itf<<8)
  static uint8_t id[256];
  tusb_control_request_t req = {};
  req.bmRequestType = 0xA1; req.bRequest = 0; req.wValue = 0; req.wIndex = 0; req.wLength = sizeof id;
  tuh_xfer_t x = {};
  // find our daddr: TinyUSB numbers the first device 1
  x.daddr = 1; x.ep_addr = 0; x.setup = &req; x.buffer = id; x.complete_cb = nullptr; x.user_data = 0;
  printerUsb.frame_irq(true);
  bool ok = tuh_control_xfer(&x);
  printerUsb.frame_irq(false);
  if (ok && x.result == XFER_RESULT_SUCCESS && x.actual_len > 2) {
    size_t n = (id[0] << 8 | id[1]);
    if (n > x.actual_len) n = x.actual_len;
    Serial.printf("device id (%u): %.*s\r\n", (unsigned)n, (int)(n - 2), (const char*)id + 2);
  } else {
    Serial.printf("GET_DEVICE_ID failed (ok %d result %d)\r\n", ok, x.result);
  }
}

// Render text into a 1-bit bitmap the height of the tape's printable area.
static uint8_t* render_text(const char* text, int h, uint16_t& w_out, bool orient) {
  M5Canvas c;
  c.setColorDepth(1);
  c.setFont(&fonts::FreeSansBold18pt7b);
  float scale = 1.0f;
  int fh = c.fontHeight();
  if (fh > 0) scale = (float)(h - 4) / fh;
  if (scale > 1.6f) scale = 1.6f;
  c.setTextSize(scale);
  int tw = c.textWidth(text) + 8;
  if (orient) tw += h + 8;
  if (tw < 16) tw = 16;
  if (!c.createSprite(tw, h)) return nullptr;
  c.fillSprite(0);
  c.setTextColor(1);
  c.setTextDatum(middle_left);
  int x0 = 4;
  if (orient) {
    // arrow pointing up at the left, plus a bar along the top edge
    c.fillTriangle(4, h - 4, 4 + h / 2, 2, 4 + h - 4, h - 4, 1);
    c.fillRect(0, 0, tw, 2, 1);
    x0 += h + 4;
  }
  c.drawString(text, x0, h / 2);
  size_t stride = (tw + 7) / 8;
  uint8_t* bits = (uint8_t*)calloc(stride * h, 1);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < tw; ++x)
      if (c.readPixel(x, y)) bits[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
  c.deleteSprite();
  w_out = tw;
  return bits;
}

static void print_text(const char* text, int copies, bool orient) {
  if (!request_status()) return;
  const pt::TapeGeom* g = pt::tape_geom(g_st.width_mm());
  if (!g) { Serial.printf("unknown tape width %u\r\n", g_st.width_mm()); return; }
  uint16_t w = 0;
  uint8_t* bits = render_text(text, g->print_pins, w, orient);
  if (!bits) { Serial.println("render failed"); return; }
  pt::Bitmap bm; bm.width = w; bm.height = g->print_pins; bm.bits = bits;
  pt::Page pages[10];
  if (copies < 1) copies = 1;
  if (copies > 10) copies = 10;
  static uint8_t* extra[10] = {nullptr};
  pages[0] = bm.page();
  for (int i = 1; i < copies; ++i) {
    char t[64]; snprintf(t, sizeof t, "%s%d", text, i + 1);
    uint16_t w2 = 0;
    extra[i] = render_text(t, g->print_pins, w2, false);
    pt::Bitmap b2; b2.width = w2; b2.height = g->print_pins; b2.bits = extra[i];
    pages[i] = b2.page();
  }
  if (copies > 1) {  // first label gets the 1 suffix too
    char t[64]; snprintf(t, sizeof t, "%s1", text);
    free(bits); bits = render_text(t, g->print_pins, w, false);
    bm.width = w; bm.bits = bits; pages[0] = bm.page();
  }
  // Stream through a 512-byte buffer.
  static uint8_t out[512];
  size_t used = 0;
  size_t total = 0;
  bool first = true;
  auto sink = [&](const uint8_t* p, size_t n) -> bool {
    total += n;
    if (g_dry) {
      for (size_t i = 0; i < n; ++i) { Serial.printf("%02X", p[i]); if ((total - n + i) % 64 == 63) Serial.print("\r\nHEX:"); }
      return true;
    }
    if (first && g_init_pause && n == 100) {
      // invalidate goes out alone, then ESC @ is sent with the next piece; pause after both
      first = false;
      if (!printerUsb.write(p, n, 10000)) return false;
      return true;
    }
    if (g_init_pause && total == 102) {
      if (!printerUsb.write(p, n, 10000)) return false;
      delay(g_init_pause);
      return true;
    }
    while (n) {
      size_t k = sizeof out - used < n ? sizeof out - used : n;
      memcpy(out + used, p, k); used += k; p += k; n -= k;
      if (used == sizeof out) { if (!printerUsb.write(out, used, 10000)) return false; used = 0; }
    }
    return true;
  };
  uint32_t t0 = millis();
  tapedeck_out_naks = 0;
  if (g_dry) Serial.print("HEX:");
  if (g_lead) {
    pt::Page orig = pages[0];
    int lead = g_lead;
    pages[0].width = orig.width + lead;
    pages[0].column = [orig, lead](uint32_t x, uint8_t* out) {
      if ((int)x < lead) { memset(out, 0, pt::Page::column_bytes(orig.height)); return true; }
      return orig.column(x - lead, out);
    };
  }
  bool ok = pt::encode_job(*g, g_st.media_type(), pages, copies, g_opt, sink);
  if (g_dry) Serial.println("\r\nEND");
  if (ok && used && !g_dry) ok = printerUsb.write(out, used, 10000);
  free(bits);
  for (int i = 1; i < copies; ++i) { free(extra[i]); extra[i] = nullptr; }
  Serial.printf("OUT NAKs during send: %lu\r\n", (unsigned long)tapedeck_out_naks);
  Serial.printf("job %s: %u bytes, %u dots long x%d, sent in %lu ms\r\n", ok ? "sent" : "FAILED",
                (unsigned)total, w, copies, millis() - t0);
  // Watch the status blocks the printer sends back while it works.
  if (g_dry) return;
  uint32_t t1 = millis();
  while (millis() - t1 < 30000) {
    pt::Status s;
    if (read_status(s, 1000)) {
      Serial.printf("+%lu ms: ", millis() - t1);
      print_status(s);
      if (s.status_type() == pt::ST_PRINT_DONE && s.phase() == 0) break;
      if (s.status_type() == pt::ST_ERROR) break;
    }
  }
}

static void handle(String line) {
  line.trim();
  if (!line.length()) return;
  char c = line[0];
  String arg = line.length() > 2 ? line.substring(2) : String();
  if (!printerUsb.ready() && !strchr("cfrxpl", c)) { Serial.println("no printer"); return; }
  switch (c) {
    case 's': request_status(); break;
    case 'i': dump_info(); break;
    case 't': print_text(arg.length() ? arg.c_str() : "TapeDeck", 1, false); break;
    case 'o': print_text(arg.length() ? arg.c_str() : "TOP", 1, true); break;
    case 'm': {
      int n = arg.toInt();
      int sp = arg.indexOf(' ');
      print_text(sp > 0 ? arg.substring(sp + 1).c_str() : "copy", n, false);
      break;
    }
    case 'r': {
      extern uint8_t tuh_max3421_reg_read(uint8_t rhport, uint8_t reg, bool in_isr);
      uint8_t hrsl = tuh_max3421_reg_read(0, 31 << 3, false);
      uint8_t mode = tuh_max3421_reg_read(0, 27 << 3, false);
      uint8_t hirq = tuh_max3421_reg_read(0, 25 << 3, false);
      uint8_t iopins1 = tuh_max3421_reg_read(0, 20 << 3, false);
      pinMode(34, INPUT);
      extern bool tuh_max3421_reg_write(uint8_t rhport, uint8_t reg, uint8_t data, bool in_isr);
      uint8_t hien = tuh_max3421_reg_read(0, 26 << 3, false);
      uint8_t cpuctl = tuh_max3421_reg_read(0, 16 << 3, false);
      uint8_t pinctl = tuh_max3421_reg_read(0, 17 << 3, false);
      Serial.printf("HIEN %02X CPUCTL %02X PINCTL %02X  G34 %d G35 %d\r\n", hien, cpuctl, pinctl, digitalRead(34), digitalRead(35));
      tuh_max3421_reg_write(0, (16 << 3) | 2, 0x00, false);  // CPUCTL IE off
      delay(2);
      Serial.printf("IE off: G34 %d G35 %d\r\n", digitalRead(34), digitalRead(35));
      tuh_max3421_reg_write(0, (16 << 3) | 2, cpuctl, false);
      delay(2);
      Serial.printf("G34 %d G35 %d\r\n", digitalRead(34), digitalRead(35));
      Serial.printf("HRSL %02X (J %d K %d) MODE %02X HIRQ %02X IOPINS1 %02X\r\n", hrsl, !!(hrsl & 0x40), !!(hrsl & 0x80), mode, hirq, iopins1);
      break;
    }
    case 'l': g_lead = arg.toInt(); Serial.printf("lead %d\r\n", g_lead); break;
    case 'x': g_dry = arg.toInt(); Serial.printf("dry %d\r\n", g_dry); break;
    case 'p': g_init_pause = arg.toInt(); Serial.printf("init pause %d ms\r\n", g_init_pause); break;
    case 'c': g_opt.chain = arg.toInt(); Serial.printf("chain %d\r\n", g_opt.chain); break;
    case 'f': g_opt.flip_pins = arg.toInt(); Serial.printf("flip %d\r\n", g_opt.flip_pins); break;
    default: Serial.println("?"); break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\r\nTapeDeck probe");
  g_cs = find_max3421_cs();
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Speaker.end();
  pinMode(25, OUTPUT); digitalWrite(25, LOW);
  lcd(g_cs < 0 ? "USB Module not found" : "waiting for printer");
  if (g_cs >= 0 && !printerUsb.begin(g_cs, 34)) Serial.println("[usb] host init failed");
}

void loop() {
  static uint32_t gen = 0;
  static String buf;
  printerUsb.pump(1);
  if (printerUsb.generation() != gen) {
    gen = printerUsb.generation();
    if (printerUsb.ready()) {
      Serial.printf("[usb] printer ready: %04X:%04X %s %s\r\n", printerUsb.vid(), printerUsb.pid(),
                    printerUsb.manufacturer(), printerUsb.product());
      dump_info();
      request_status();
    } else if (printerUsb.attached()) {
      Serial.printf("[usb] device %04X:%04X has no printer interface\r\n", printerUsb.vid(), printerUsb.pid());
      dump_info();
    } else {
      Serial.println("[usb] detached");
      lcd("printer unplugged");
    }
  }
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { handle(buf); buf = ""; }
    else buf += ch;
  }
}
