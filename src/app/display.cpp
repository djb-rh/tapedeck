#include "display.h"
#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>
#include "app.h"
#include "jobs.h"
#include "printer.h"

Display display;

static constexpr int W = 320, H = 240;
static constexpr uint32_t C_BG = 0x101418, C_PANEL = 0x1d232a, C_TEXT = 0xe8ecef, C_DIM = 0x8a96a3,
                          C_OK = 0x3ccf6e, C_WARN = 0xf2b134, C_ERR = 0xff5a5a, C_ACCENT = 0x4fa3ff;
// y ranges
static constexpr int HDR_H = 22, TAPE_Y = 26, TAPE_H = 66, JOB_Y = 96, JOB_H = 114, FOOT_Y = 214;

static uint32_t rgb(uint32_t c) { return c; }

static uint32_t tape_rgb(uint8_t c) {
  switch (c) {
    case 0x01: case 0x20: case 0x70: case 0x90: return 0xfbfbf6;
    case 0x03: case 0x09: case 0x21: return 0xdfe6e9;
    case 0x04: case 0x31: return 0xd8262c;
    case 0x05: case 0x30: return 0x1f5fb8;
    case 0x06: case 0x60: case 0x91: return 0xf7d417;
    case 0x07: return 0x2f9a4a;
    case 0x08: return 0x1a1a1a;
    case 0x22: case 0x24: return 0xc9cccf;
    case 0x23: return 0xcfae5a;
    case 0x40: return 0xff7a1a;
    case 0x41: return 0xe6ff3a;
    case 0x50: case 0x61: return 0xf06aa7;
    case 0x51: return 0xc4c4c4;
    case 0x52: return 0x9fdc4a;
    case 0x62: return 0x4fa6ff;
    default: return 0xf4f4f0;
  }
}
static uint32_t ink_rgb(uint8_t c) {
  switch (c) {
    case 0x01: return 0xffffff;
    case 0x04: return 0xd8262c;
    case 0x05: case 0x62: return 0x1f5fb8;
    case 0x0A: return 0xcfae5a;
    default: return 0x111111;
  }
}

void Display::begin() {
  // Keep M5Unified's board default (landscape on the Core Basic); some panel
  // revisions report portrait for rotation 1, so only fix it if needed.
  Serial.printf("[lcd] default rotation %d: %dx%d\r\n", M5.Display.getRotation(), M5.Display.width(), M5.Display.height());
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation((M5.Display.getRotation() + 1) & 3);
  M5.Display.fillScreen(rgb(C_BG));
  M5.Display.setTextWrap(false);
  last_activity_ms_ = millis();
}

void Display::wake() {
  last_activity_ms_ = millis();
  if (!lit_) {
    lit_ = true;
    M5.Display.setBrightness(settings.brightness);
  }
}

// ------------------------------------------------------------- header

void Display::draw_header(bool force) {
  String right;
  if (app.sta_connected) right = settings.hostname + ".local  " + WiFi.localIP().toString();
  else if (app.ap_mode) right = "Setup: " + app.ap_ssid;
  else right = "Wi-Fi connecting...";
  if (!force && right == last_header_) return;
  last_header_ = right;
  auto& d = M5.Display;
  d.fillRect(0, 0, W, HDR_H, rgb(C_PANEL));
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(rgb(C_TEXT));
  d.setTextDatum(middle_left);
  d.drawString("TapeDeck", 6, HDR_H / 2 + 1);
  d.setFont(&fonts::Font2);
  d.setTextColor(app.sta_connected ? rgb(C_DIM) : rgb(C_WARN));
  d.setTextDatum(middle_right);
  d.drawString(right.c_str(), W - 6, HDR_H / 2 + 1);
}

// ------------------------------------------------------------- tape + status

void Display::draw_tape(bool force) {
  PrinterSnapshot s = printer.snapshot();
  if (!force && s.seq == last_seq_) return;
  last_seq_ = s.seq;
  auto& d = M5.Display;
  d.fillRect(0, TAPE_Y, W, TAPE_H, rgb(C_BG));
  // cassette swatch
  const int sx = 8, sy = TAPE_Y + 4, sw = 70, sh = TAPE_H - 8;
  if (s.status_valid && s.st.width_mm()) {
    uint32_t tc = tape_rgb(s.tape_color), ic = ink_rgb(s.ink_color);
    int band = 12 + s.st.width_mm() * 1.4;  // tape band height scaled by width
    if (band > sh) band = sh;
    d.fillRoundRect(sx, sy, sw, sh, 6, rgb(0x2b333c));
    d.fillRect(sx, sy + (sh - band) / 2, sw, band, tc);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextColor(ic);
    d.setTextDatum(middle_center);
    d.drawString("Aa", sx + sw / 2, sy + sh / 2 + 1);
  } else {
    d.drawRoundRect(sx, sy, sw, sh, 6, rgb(0x3a444f));
    d.setFont(&fonts::Font2);
    d.setTextColor(rgb(C_DIM));
    d.setTextDatum(middle_center);
    d.drawString("no tape", sx + sw / 2, sy + sh / 2);
  }
  // text
  const int tx = sx + sw + 10;
  d.setTextDatum(top_left);
  char l1[48], l2[64];
  uint32_t scol = rgb(C_OK);
  const char* state = "Ready";
  char statebuf[96];
  switch (s.state) {
    case PState::NO_MODULE: state = "USB Module not found"; scol = rgb(C_ERR); break;
    case PState::NO_PRINTER: state = "Printer not connected"; scol = rgb(C_WARN); break;
    case PState::PRINTING:
      snprintf(statebuf, sizeof statebuf, "Printing %d of %d", s.pages_done + 1 > s.pages_total ? s.pages_total : s.pages_done + 1,
               s.pages_total);
      state = statebuf; scol = rgb(C_ACCENT); break;
    case PState::ERROR:
      snprintf(statebuf, sizeof statebuf, "%s", s.printer_error[0] ? s.printer_error : "Printer error");
      state = statebuf; scol = rgb(C_ERR); break;
    case PState::IDLE:
      if (s.queued) { snprintf(statebuf, sizeof statebuf, "%d queued", s.queued); state = statebuf; scol = rgb(C_ACCENT); }
      else if (s.label_held) { state = "Ready - last label held"; scol = rgb(C_WARN); }
      else if (!s.colors_confirmed) { state = "New tape? Hold B: colours"; scol = rgb(C_WARN); }
      break;
  }
  if (s.status_valid && s.st.width_mm()) {
    snprintf(l1, sizeof l1, "%u mm %s", s.st.width_mm(), s.st.media_type() == 0x01 || s.st.media_type() == 0x03 ? "tape" : pt::media_type_name(s.st.media_type()));
    snprintf(l2, sizeof l2, "%s on %s%s", pt::text_color_name(s.ink_color), pt::tape_color_name(s.tape_color), s.colors_confirmed ? "" : "?");
  } else if (s.state == PState::NO_PRINTER || s.state == PState::NO_MODULE) {
    snprintf(l1, sizeof l1, "%s", s.state == PState::NO_MODULE ? "No USB host" : "No printer");
    snprintf(l2, sizeof l2, "%s", s.state == PState::NO_MODULE ? "Check module + DIP switches" : "Plug in + power on the P-touch");
  } else {
    snprintf(l1, sizeof l1, "%s", s.product[0] ? s.product : "Printer");
    snprintf(l2, sizeof l2, "No tape loaded");
  }
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(rgb(C_TEXT));
  d.drawString(l1, tx, TAPE_Y + 3);
  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextColor(rgb(C_DIM));
  d.drawString(l2, tx, TAPE_Y + 27);
  d.setTextColor(scol);
  d.drawString(state, tx, TAPE_Y + 46);
  if (s.state != PState::IDLE || s.queued) wake();
}

// ------------------------------------------------------------- job preview

static String ago(uint32_t created) {
  time_t now = time(nullptr);
  if (!created || now < 1600000000) return "";
  long s = (long)(now - created);
  char b[24];
  if (s < 60) snprintf(b, sizeof b, "just now");
  else if (s < 3600) snprintf(b, sizeof b, "%ldm ago", s / 60);
  else if (s < 86400) snprintf(b, sizeof b, "%ldh ago", s / 3600);
  else snprintf(b, sizeof b, "%ldd ago", s / 86400);
  return b;
}

// Renders the job's pages (one copy) into a sprite, scaled to fit.
static void draw_preview(const JobEntry& e, int x0, int y0, int pw, int ph, uint32_t tape, uint32_t ink) {
  M5Canvas c(&M5.Display);
  c.setColorDepth(8);
  if (!c.createSprite(pw, ph)) return;
  c.fillSprite(rgb(C_BG));
  File f = LittleFS.open(JobStore::path(e.id), "r");
  if (!f) { c.deleteSprite(); return; }
  const int gap = 6;
  const int npages = e.hdr.pages;
  // scale: fit tape height into ph, and total length into pw
  const int tape_h = e.max_height ? e.max_height : 64;
  float sc = (float)ph / tape_h;
  float total = e.total_cols * sc + gap * (npages - 1);
  if (total > pw) sc = (pw - gap * (npages - 1)) / (float)e.total_cols;
  if (sc <= 0) sc = 0.01f;
  int dy = (ph - (int)(tape_h * sc)) / 2;
  if (dy < 0) dy = 0;
  int x = 0;
  uint32_t off = tdj::kHeaderSize;
  uint8_t col[16];
  for (int p = 0; p < npages && x < pw; ++p) {
    uint8_t phd[4];
    tdj::PageInfo pi;
    f.seek(off);
    if (f.read(phd, 4) != 4 || !tdj::read_page_header(phd, pi)) break;
    const int cb = (pi.height + 7) / 8;
    int dw = (int)(pi.width * sc + 0.5f);
    if (dw < 1) dw = 1;
    int dh = (int)(tape_h * sc + 0.5f);
    c.fillRect(x, dy, dw, dh, tape);
    int yoff = (tape_h - pi.height) / 2;
    for (int dx = 0; dx < dw && x + dx < pw; ++dx) {
      uint32_t sx = (uint32_t)(dx / sc);
      if (sx >= pi.width) sx = pi.width - 1;
      f.seek(off + 4 + sx * cb);
      f.read(col, cb);
      for (int yy = 0; yy < dh; ++yy) {
        int sy = (int)(yy / sc) - yoff;
        if (sy < 0 || sy >= pi.height) continue;
        if (col[sy >> 3] & (0x80 >> (sy & 7))) c.drawPixel(x + dx, dy + yy, ink);
      }
    }
    x += dw + gap;
    off += 4 + pi.bytes();
  }
  f.close();
  if (x - gap > pw) {  // clipped: fade marker
    c.fillRect(pw - 14, 0, 14, ph, rgb(C_BG));
    c.setTextColor(rgb(C_DIM));
    c.setTextDatum(middle_center);
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.drawString(">", pw - 7, ph / 2);
  }
  c.pushSprite(x0, y0);
  c.deleteSprite();
}

void Display::draw_job(bool force) {
  int n = jobs.count();
  if (sel_ >= n) sel_ = n ? n - 1 : 0;
  JobEntry e;
  bool have = jobs.at(sel_, e);
  PrinterSnapshot s = printer.snapshot();
  uint8_t tc = s.status_valid ? s.tape_color : 0x01, ic = s.status_valid ? s.ink_color : 0x08;
  // signature: selection, job, count, printed count, tape colours, time bucket
  uint32_t sig = (have ? e.id * 2654435761u : 1) ^ (n << 24) ^ (sel_ << 16) ^ (e.printed << 8) ^ (tc << 4) ^ ic ^
                 (uint32_t)(time(nullptr) / 60);
  if (!force && sig == last_job_sig_) return;
  last_job_sig_ = sig;
  sel_id_ = have ? e.id : 0;
  auto& d = M5.Display;
  d.fillRect(0, JOB_Y, W, JOB_H, rgb(C_BG));
  d.drawFastHLine(8, JOB_Y, W - 16, rgb(0x2b333c));
  d.setTextDatum(top_left);
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_DIM));
  if (!have) {
    d.drawString("No jobs yet", 8, JOB_Y + 8);
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextColor(rgb(C_TEXT));
    String url = app_ip().length() ? "http://" + (app.sta_connected ? settings.hostname + ".local" : app_ip()) + "/" : String("");
    d.drawString("Design labels in a browser:", 8, JOB_Y + 34);
    d.setTextColor(rgb(C_ACCENT));
    d.drawString(url.c_str(), 8, JOB_Y + 56);
    return;
  }
  char hdr[48];
  snprintf(hdr, sizeof hdr, "Job %d of %d", sel_ + 1, n);
  d.drawString(hdr, 8, JOB_Y + 4);
  d.setTextDatum(top_right);
  String meta = "#" + String(e.id);
  String a = ago(e.hdr.created);
  if (a.length()) meta += "  " + a;
  d.drawString(meta.c_str(), W - 8, JOB_Y + 4);
  draw_preview(e, 8, JOB_Y + 22, W - 16, 50, tape_rgb(tc), ink_rgb(ic));
  d.setTextDatum(top_left);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(rgb(C_TEXT));
  String title = e.hdr.title;
  while (title.length() > 3 && d.textWidth(title.c_str()) > W - 16) title = title.substring(0, title.length() - 4) + "...";
  d.drawString(title.c_str(), 8, JOB_Y + 78);
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_DIM));
  char info[96];
  float mm = e.total_cols * 25.4f / pt::kDpi;
  snprintf(info, sizeof info, "%u label%s%s, %.0f mm, for %u mm tape%s", e.hdr.pages, e.hdr.pages == 1 ? "" : "s",
           e.hdr.copies > 1 ? (String(" x") + e.hdr.copies).c_str() : "", mm, e.hdr.tape_mm,
           e.printed ? (", printed " + String(e.printed) + "x").c_str() : "");
  d.drawString(info, 8, JOB_Y + 98);
}

// ------------------------------------------------------------- footer

void Display::draw_footer(bool force) {
  String f;
  uint32_t now = millis();
  if (toast_until_ && now < toast_until_) f = "!" + toast_;
  else if (confirm_until_ && now < confirm_until_) f = "?Press again to print";
  else f = "Newer|Print|Older";
  if (!force && f == last_footer_) return;
  last_footer_ = f;
  auto& d = M5.Display;
  d.fillRect(0, FOOT_Y, W, H - FOOT_Y, rgb(C_PANEL));
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  const int cy = FOOT_Y + (H - FOOT_Y) / 2 + 1;
  if (f[0] == '!' || f[0] == '?') {
    d.setTextColor(f[0] == '?' ? rgb(C_WARN) : rgb(C_TEXT));
    d.drawString(f.c_str() + 1, W / 2, cy);
    return;
  }
  // button labels over the three buttons (x = 65, 160, 255)
  d.setTextColor(rgb(C_TEXT));
  d.drawString("< Newer", 65, cy);
  d.setTextColor(sel_id_ ? rgb(C_ACCENT) : rgb(C_DIM));
  d.drawString("Print", 160, cy);
  d.setTextColor(rgb(C_TEXT));
  d.drawString("Older >", 255, cy);
}

// ------------------------------------------------------------- setup screen (AP mode, no network yet)

void Display::draw_setup(bool force) {
  if (!force) return;
  auto& d = M5.Display;
  d.fillRect(0, JOB_Y, W, FOOT_Y - JOB_Y, rgb(C_BG));
  d.drawFastHLine(8, JOB_Y, W - 16, rgb(0x2b333c));
  String qr = "WIFI:S:" + app.ap_ssid + ";T:nopass;;";
  d.qrcode(qr.c_str(), 8, JOB_Y + 6, 108, 3);
  d.setTextDatum(top_left);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(rgb(C_TEXT));
  d.drawString("Wi-Fi setup", 126, JOB_Y + 8);
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_DIM));
  d.drawString("1. Scan or join Wi-Fi", 126, JOB_Y + 32);
  d.setTextColor(rgb(C_ACCENT));
  d.drawString(app.ap_ssid.c_str(), 140, JOB_Y + 48);
  d.setTextColor(rgb(C_DIM));
  d.drawString("2. The setup page opens, or", 126, JOB_Y + 68);
  d.drawString("   browse to", 126, JOB_Y + 84);
  d.setTextColor(rgb(C_ACCENT));
  d.drawString(("http://" + WiFi.softAPIP().toString() + "/setup").c_str(), 126, JOB_Y + 100);
  d.fillRect(0, FOOT_Y, W, H - FOOT_Y, rgb(C_PANEL));
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_DIM));
  d.setTextDatum(middle_center);
  d.drawString("Any button: job history  -  hold B: this screen", W / 2, FOOT_Y + (H - FOOT_Y) / 2);
  last_footer_ = "";
}

// ------------------------------------------------------------- info screen (hold B)
// QR code for the web editor, network details, and the tape colour picker
// (A / C step through recent cassettes, then every preset; B saves).

static String choice_name(uint8_t tape, uint8_t ink) {
  int p = pt::find_color_preset(tape, ink);
  if (p >= 0) return pt::kColorPresets[p].name;
  return String(pt::text_color_name(ink)) + " on " + pt::tape_color_name(tape);
}

void Display::open_info() {
  info_ = true;
  info_until_ = millis() + 60000;
  PrinterSnapshot s = printer.snapshot();
  info_have_tape_ = s.status_valid && s.st.width_mm();
  // recent cassettes first (this width + type at the top), then the presets
  TapeChoice rec[kRecentMax];
  int nr = printer.recent(rec, kRecentMax);
  n_choices_ = 0;
  for (int i = 0; i < nr; ++i) {
    bool dup = false;
    for (int k = 0; k < n_choices_; ++k)
      if (choices_[k].tape == rec[i].tape && choices_[k].ink == rec[i].ink) dup = true;
    if (dup) continue;
    Choice& ch = choices_[n_choices_++];
    ch.tape = rec[i].tape; ch.ink = rec[i].ink;
    ch.recent = true;
    ch.same_type = rec[i].w == s.st.width_mm() && rec[i].m == s.st.media_type();
    ch.width = rec[i].w;
  }
  for (int i = 0; i < pt::kColorPresetCount && n_choices_ < (int)(sizeof choices_ / sizeof choices_[0]); ++i) {
    bool dup = false;
    for (int k = 0; k < n_choices_; ++k)
      if (choices_[k].tape == pt::kColorPresets[i].tape && choices_[k].ink == pt::kColorPresets[i].ink) dup = true;
    if (dup) continue;
    Choice& ch = choices_[n_choices_++];
    ch.tape = pt::kColorPresets[i].tape; ch.ink = pt::kColorPresets[i].ink;
    ch.recent = false; ch.same_type = false; ch.width = 0;
  }
  // start on the colours currently assumed
  info_preset_ = 0;
  for (int k = 0; k < n_choices_; ++k)
    if (choices_[k].tape == s.tape_color && choices_[k].ink == s.ink_color) { info_preset_ = k; break; }
  info_dirty_ = true;
}

void Display::close_info(bool save) {
  if (save && info_have_tape_) {
    String err;
    const Choice& p = choices_[info_preset_];
    String name = choice_name(p.tape, p.ink);
    if (printer.set_colors(p.tape, p.ink, err)) {
      events.add("Tape colours set on the Core: %s", name.c_str());
      toast_ = "Tape set to " + name;
    } else {
      toast_ = err;
    }
    toast_until_ = millis() + 2500;
  }
  info_ = false;
  force_all_ = true;
}

void Display::draw_info() {
  if (!info_dirty_) return;
  info_dirty_ = false;
  auto& d = M5.Display;
  d.fillRect(0, TAPE_Y, W, FOOT_Y - TAPE_Y, rgb(C_BG));
  String url = app.sta_connected ? "http://" + WiFi.localIP().toString() + "/" : String("");
  String qr = app.sta_connected ? url : "WIFI:S:" + app.ap_ssid + ";T:nopass;;";
  d.qrcode(qr.c_str(), 8, TAPE_Y + 2, 112, 3);
  const int tx = 128;
  d.setTextDatum(top_left);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(rgb(C_TEXT));
  d.drawString(app.sta_connected ? "Label editor" : "Join to set up", tx, TAPE_Y + 2);
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_ACCENT));
  if (app.sta_connected) {
    d.drawString((settings.hostname + ".local").c_str(), tx, TAPE_Y + 24);
    d.drawString(url.c_str(), tx, TAPE_Y + 40);
    d.setTextColor(rgb(C_DIM));
    d.drawString((WiFi.SSID() + "  " + String(WiFi.RSSI()) + " dBm").c_str(), tx, TAPE_Y + 58);
  } else {
    d.drawString(app.ap_ssid.c_str(), tx, TAPE_Y + 24);
    d.drawString(("http://" + WiFi.softAPIP().toString() + "/setup").c_str(), tx, TAPE_Y + 40);
    d.setTextColor(rgb(C_DIM));
    d.drawString(settings.wifi_ssid.length() ? ("joining " + settings.wifi_ssid).c_str() : "no network saved", tx, TAPE_Y + 58);
  }
  d.setTextColor(rgb(C_DIM));
  d.drawString("TapeDeck " TAPEDECK_VERSION, tx, TAPE_Y + 76);
  d.drawString("Setup: /setup", tx, TAPE_Y + 92);
  // colour picker
  const int py = TAPE_Y + 122;
  d.drawFastHLine(8, py - 6, W - 16, rgb(0x2b333c));
  d.setFont(&fonts::Font2);
  d.setTextColor(rgb(C_DIM));
  d.drawString("Tape colours (the printer can't tell)", 8, py);
  if (!info_have_tape_) {
    d.setTextColor(rgb(C_WARN));
    d.drawString("No tape loaded", 8, py + 22);
    return;
  }
  const Choice& p = choices_[info_preset_];
  const int sx = 8, sy = py + 20, sw = 70, sh = 36;
  d.fillRoundRect(sx, sy, sw, sh, 5, rgb(0x2b333c));
  d.fillRect(sx, sy + 6, sw, sh - 12, tape_rgb(p.tape));
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(ink_rgb(p.ink));
  d.setTextDatum(middle_center);
  d.drawString("Aa", sx + sw / 2, sy + sh / 2 + 1);
  d.setTextDatum(top_left);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(rgb(C_TEXT));
  d.drawString(choice_name(p.tape, p.ink).c_str(), sx + sw + 10, sy + 2);
  d.setFont(&fonts::Font2);
  char n[48];
  if (p.recent && p.same_type) { d.setTextColor(rgb(C_OK)); snprintf(n, sizeof n, "Used before, this tape type"); }
  else if (p.recent) { d.setTextColor(rgb(C_ACCENT)); snprintf(n, sizeof n, "Used before, %u mm tape", p.width); }
  else { d.setTextColor(rgb(C_DIM)); snprintf(n, sizeof n, "Preset"); }
  d.drawString(n, sx + sw + 10, sy + 26);
  d.setTextColor(rgb(C_DIM));
  d.setTextDatum(top_right);
  snprintf(n, sizeof n, "%d/%d", info_preset_ + 1, n_choices_);
  d.drawString(n, W - 8, sy + 26);
  d.setTextDatum(top_left);
}

void Display::draw_info_footer() {
  String f = info_have_tape_ ? "info:colours" : "info:back";
  if (f == last_footer_) return;
  last_footer_ = f;
  auto& d = M5.Display;
  d.fillRect(0, FOOT_Y, W, H - FOOT_Y, rgb(C_PANEL));
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  const int cy = FOOT_Y + (H - FOOT_Y) / 2 + 1;
  d.setTextColor(rgb(C_TEXT));
  if (info_have_tape_) {
    d.drawString("< Colour", 65, cy);
    d.setTextColor(rgb(C_ACCENT));
    d.drawString("Save", 160, cy);
    d.setTextColor(rgb(C_TEXT));
    d.drawString("Colour >", 255, cy);
  } else {
    d.drawString("Back", 160, cy);
  }
}

// ------------------------------------------------------------- input

void Display::do_print() {
  if (!sel_id_) return;
  String err;
  if (printer.enqueue(sel_id_, err)) {
    toast_ = "Printing job #" + String(sel_id_);
  } else {
    toast_ = err;
  }
  toast_until_ = millis() + 2500;
}

void Display::press(int btn, bool hold) {
  wake();
  if (info_) {
    info_until_ = millis() + 60000;
    if (btn == 1) { close_info(!hold); return; }
    if (!info_have_tape_) { close_info(false); return; }
    if (n_choices_) info_preset_ = (info_preset_ + (btn == 0 ? n_choices_ - 1 : 1)) % n_choices_;
    info_dirty_ = true;
    return;
  }
  if (btn == 1 && hold) { confirm_until_ = 0; open_info(); return; }
  if (!setup_dismissed_ && app.ap_mode && !app.sta_connected && settings.wifi_ssid.isEmpty()) {
    setup_dismissed_ = true;  // first press on the setup screen shows the job history
    return;
  }
  int n = jobs.count();
  if (btn == 0) {
    if (hold) { printer.refresh_status(); toast_ = "Refreshing printer status"; toast_until_ = millis() + 1500; return; }
    if (sel_ > 0) --sel_;
    confirm_until_ = 0;
  } else if (btn == 2) {
    if (hold) sel_ = 0;
    else if (sel_ + 1 < n) ++sel_;
    confirm_until_ = 0;
  } else if (btn == 1) {
    if (confirm_until_ && millis() < confirm_until_) { confirm_until_ = 0; do_print(); }
    else if (sel_id_) confirm_until_ = millis() + 3000;
  }
}

void Display::handle_buttons() {
  bool any = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
  if (any && !lit_) {  // first press only wakes the screen
    wake();
    M5.BtnA.wasClicked(); M5.BtnB.wasClicked(); M5.BtnC.wasClicked();
    return;
  }
  if (M5.BtnA.wasHold()) press(0, true);
  else if (M5.BtnA.wasClicked()) press(0);
  if (M5.BtnB.wasHold()) press(1, true);
  else if (M5.BtnB.wasClicked()) press(1);
  if (M5.BtnC.wasHold()) press(2, true);
  else if (M5.BtnC.wasClicked()) press(2);
}

void Display::task() {
  static uint32_t last = 0;
  handle_buttons();
  uint32_t now = millis();
  if (now - app.last_web_ms < 1000) wake();
  if (now - last < 100) return;
  last = now;
  bool setup_mode = app.ap_mode && !app.sta_connected && settings.wifi_ssid.isEmpty() && !setup_dismissed_;
  bool force = force_all_ || setup_mode != was_setup_;
  force_all_ = false;
  was_setup_ = setup_mode;
  if (force) M5.Display.fillScreen(rgb(C_BG));
  draw_header(force);
  if (info_) {
    if (now > info_until_) { close_info(false); return; }
    if (force) { info_dirty_ = true; last_footer_ = ""; }
    draw_info();
    draw_info_footer();
    return;
  }
  draw_tape(force);
  if (setup_mode) draw_setup(force);
  else {
    draw_job(force);
    draw_footer(force);
  }
  if (settings.disp_timeout_s && lit_ && now - last_activity_ms_ > settings.disp_timeout_s * 1000UL) {
    lit_ = false;
    M5.Display.setBrightness(0);
  }
}

// ------------------------------------------------------------- screenshot

void Display::screenshot() {
  // "SHOT w h" then rows of RLE pairs (count hex, rgb565 hex), "END".
  auto& d = M5.Display;
  Serial.printf("SHOT %d %d\r\n", W, H);
  uint16_t row[W];
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) { uint16_t v = d.readPixel(x, y); row[x] = (v >> 8) | (v << 8); }
    String line = "R";
    int x = 0;
    while (x < W) {
      int k = 1;
      while (x + k < W && row[x + k] == row[x] && k < 255) ++k;
      char b[12];
      snprintf(b, sizeof b, " %x:%04x", k, row[x]);
      line += b;
      x += k;
    }
    Serial.println(line);
  }
  Serial.println("END");
}
