#include "render.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <M5GFX.h>
#include <vector>
#include "jobfile.h"
#include "ptouch.h"

namespace render {

using lgfx::GFXfont;

struct FontChoice {
  const GFXfont* font;
  float scale;
};

// Candidate fonts per family, smallest first. Scaled entries are 2x of the
// biggest real size (blocky but legible) for 18/24 mm tape.
static const GFXfont* kSans[] = {&fonts::DejaVu9, &fonts::DejaVu12, &fonts::DejaVu18, &fonts::DejaVu24,
                                 &fonts::DejaVu40, &fonts::DejaVu56, &fonts::DejaVu72};
static const GFXfont* kBold[] = {&fonts::FreeSansBold9pt7b, &fonts::FreeSansBold12pt7b,
                                 &fonts::FreeSansBold18pt7b, &fonts::FreeSansBold24pt7b};
static const GFXfont* kMono[] = {&fonts::FreeMonoBold9pt7b, &fonts::FreeMonoBold12pt7b,
                                 &fonts::FreeMonoBold18pt7b, &fonts::FreeMonoBold24pt7b};
static const GFXfont* kSerif[] = {&fonts::FreeSerifBold9pt7b, &fonts::FreeSerifBold12pt7b,
                                  &fonts::FreeSerifBold18pt7b, &fonts::FreeSerifBold24pt7b};

static void family(const char* name, const GFXfont* const*& list, int& n) {
  if (!name || !*name || !strcmp(name, "sans")) { list = kSans; n = 7; return; }
  if (!strcmp(name, "mono")) { list = kMono; n = 4; return; }
  if (!strcmp(name, "serif")) { list = kSerif; n = 4; return; }
  list = kBold; n = 4;  // "bold" and anything unknown
}

static int split_lines(const String& text, String* out, int cap) {
  int n = 0, start = 0;
  for (int i = 0; i <= (int)text.length() && n < cap; ++i) {
    if (i == (int)text.length() || text[i] == '\n') {
      String l = text.substring(start, i);
      l.replace("\r", "");
      out[n++] = l;
      start = i + 1;
    }
  }
  return n;
}

// Width of the widest line and height of one line for a font choice.
static void measure(M5Canvas& c, const FontChoice& f, String* lines, int n, int& w, int& lh) {
  c.setFont(f.font);
  c.setTextSize(f.scale);
  lh = c.fontHeight();
  w = 0;
  for (int i = 0; i < n; ++i) {
    int tw = c.textWidth(lines[i].c_str());
    if (tw > w) w = tw;
  }
}

struct LabelSpec {
  String text;
  String font = "bold";
  int size = 0;        // line height in dots, 0 = fit the tape
  String align = "center";
  float length_mm = 0; // 0 = as long as the text
  float pad_mm = 1.0f;
  bool frame = false;
  bool invert = false;
  int copies = 1;
};

static void spec_from(JsonObjectConst o, LabelSpec& s) {
  if (o["text"].is<const char*>()) s.text = o["text"].as<const char*>();
  if (o["lines"].is<JsonArrayConst>()) {
    s.text = "";
    for (JsonVariantConst v : o["lines"].as<JsonArrayConst>()) {
      if (s.text.length()) s.text += "\n";
      s.text += v.as<const char*>() ? v.as<const char*>() : "";
    }
  }
  if (o["font"].is<const char*>()) s.font = o["font"].as<const char*>();
  if (o["size"].is<int>()) s.size = o["size"];
  if (o["align"].is<const char*>()) s.align = o["align"].as<const char*>();
  if (o["length_mm"].is<float>()) s.length_mm = o["length_mm"];
  if (o["pad_mm"].is<float>()) s.pad_mm = o["pad_mm"];
  if (o["frame"].is<bool>()) s.frame = o["frame"];
  if (o["invert"].is<bool>()) s.invert = o["invert"];
  if (o["copies"].is<int>()) s.copies = o["copies"];
  if (s.copies < 1) s.copies = 1;
  if (s.copies > 50) s.copies = 50;
}

static bool write_page(File& f, M5Canvas& c, int w, int h) {
  uint8_t ph[4];
  tdj::write_page_header(w, h, ph);
  if (f.write(ph, 4) != 4) return false;
  const int cb = (h + 7) / 8;
  uint8_t col[16];
  for (int x = 0; x < w; ++x) {
    memset(col, 0, sizeof col);
    for (int y = 0; y < h; ++y)
      if (c.readPixel(x, y)) col[y >> 3] |= 0x80 >> (y & 7);
    if (f.write(col, cb) != (size_t)cb) return false;
  }
  return true;
}

static bool render_label(const LabelSpec& s, int H, File& f, String& err) {
  String lines[12];
  int n = split_lines(s.text, lines, 12);
  if (n == 0) { n = 1; lines[0] = ""; }
  const int pad = (int)(s.pad_mm * pt::kDpi / 25.4f + 0.5f);
  const int frame_w = s.frame ? 3 : 0;
  const int inner_h = H - 2 * (frame_w ? frame_w + 2 : 0);
  const int fixed_w = s.length_mm > 0 ? (int)(s.length_mm * pt::kDpi / 25.4f + 0.5f) : 0;

  M5Canvas c;
  c.setColorDepth(1);
  const GFXfont* const* list;
  int nf;
  family(s.font.c_str(), list, nf);
  // Try every font from the largest down; scaled-up variants of the biggest first.
  FontChoice best = {list[0], 1.0f};
  bool found = false;
  int tw = 0, lh = 0;
  FontChoice cands[16];
  int nc = 0;
  for (float sc : {2.0f, 1.5f}) cands[nc++] = {list[nf - 1], sc};
  for (int i = nf - 1; i >= 0; --i) cands[nc++] = {list[i], 1.0f};
  for (int i = 0; i < nc; ++i) {
    measure(c, cands[i], lines, n, tw, lh);
    bool fits_h = s.size > 0 ? lh <= s.size : lh * n <= inner_h;
    bool fits_w = !fixed_w || tw + 2 * pad + 2 * frame_w <= fixed_w;
    if (fits_h && fits_w) { best = cands[i]; found = true; break; }
  }
  if (!found) { best = cands[nc - 1]; measure(c, best, lines, n, tw, lh); }
  measure(c, best, lines, n, tw, lh);
  int W = fixed_w ? fixed_w : tw + 2 * pad + 2 * (frame_w ? frame_w + 2 : 0);
  if (W < 8) W = 8;
  if (W > 4000) { err = "label too long (over 56 cm)"; return false; }
  if (!c.createSprite(W, H)) { err = "out of memory rendering label"; return false; }
  const int bg = s.invert ? 1 : 0, fg = s.invert ? 0 : 1;
  c.fillSprite(bg);
  if (s.frame) {
    for (int k = 0; k < frame_w; ++k) c.drawRect(k, k, W - 2 * k, H - 2 * k, fg);
  }
  c.setFont(best.font);
  c.setTextSize(best.scale);
  c.setTextColor(fg, bg);
  int block = lh * n;
  int y0 = (H - block) / 2;
  int left = pad + (frame_w ? frame_w + 2 : 0), right = W - left;
  for (int i = 0; i < n; ++i) {
    int y = y0 + i * lh + lh / 2;
    if (s.align == "left") { c.setTextDatum(middle_left); c.drawString(lines[i].c_str(), left, y); }
    else if (s.align == "right") { c.setTextDatum(middle_right); c.drawString(lines[i].c_str(), right, y); }
    else { c.setTextDatum(middle_center); c.drawString(lines[i].c_str(), W / 2, y); }
  }
  bool ok = true;
  for (int k = 0; k < s.copies && ok; ++k) ok = write_page(f, c, W, H);
  c.deleteSprite();
  if (!ok) err = "flash write failed";
  return ok;
}

bool text_job(const char* json, size_t len, int H, int tape_mm, const char* out_path, String& err) {
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json, len);
  if (de) { err = String("bad JSON: ") + de.c_str(); return false; }
  if (H <= 0) { err = "no tape loaded (label height unknown)"; return false; }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  LabelSpec defaults;
  if (root["defaults"].is<JsonObjectConst>()) spec_from(root["defaults"], defaults);
  // Top-level label fields apply as defaults too, so {"text": "..."} works.
  spec_from(root, defaults);

  std::vector<LabelSpec> specs;
  if (root["labels"].is<JsonArrayConst>()) {
    for (JsonVariantConst v : root["labels"].as<JsonArrayConst>()) {
      LabelSpec s = defaults;
      s.copies = 1;
      if (v.is<const char*>()) s.text = v.as<const char*>();
      else spec_from(v.as<JsonObjectConst>(), s);
      specs.push_back(s);
    }
  } else if (root["text"].is<const char*>() || root["lines"].is<JsonArrayConst>()) {
    specs.push_back(defaults);
  }
  if (specs.empty()) { err = "no labels: give \"text\", \"lines\" or \"labels\""; return false; }
  int pages = 0;
  for (auto& s : specs) pages += s.copies;
  if (pages > tdj::kMaxPages) { err = "too many labels"; return false; }

  tdj::Header h;
  h.tape_mm = tape_mm;
  h.copies = root["copies"] | 1;
  if (h.copies < 1) h.copies = 1;
  if (h.copies > 99) h.copies = 99;
  h.pages = pages;
  float margin = root["margin_mm"] | 2.0f;
  h.margin_dots = (uint16_t)(margin * pt::kDpi / 25.4f + 0.5f);
  if (root["chain"] | false) h.flags |= tdj::F_CHAIN;
  String title = root["title"] | "";
  if (!title.length()) {
    title = specs[0].text;
    title.replace("\n", " / ");
    if (specs.size() > 1) title += " +" + String(specs.size() - 1);
  }
  strlcpy(h.title, title.c_str(), sizeof h.title);

  File f = LittleFS.open(out_path, "w");
  if (!f) { err = "cannot create job file"; return false; }
  uint8_t hb[tdj::kHeaderSize];
  tdj::write_header(h, hb);
  f.write(hb, sizeof hb);
  for (auto& s : specs) {
    if (!render_label(s, H, f, err)) { f.close(); LittleFS.remove(out_path); return false; }
  }
  f.close();
  return true;
}

bool pbm_job(const char* pbm_path, const char* out_path, const String& title, int copies, String& err) {
  File in = LittleFS.open(pbm_path, "r");
  if (!in) { err = "cannot open upload"; return false; }
  // Header: "P4" whitespace, comments, width, height, single whitespace.
  auto token = [&]() -> String {
    String t;
    while (in.available()) {
      int ch = in.read();
      if (ch == '#') { while (in.available() && in.read() != '\n') {} continue; }
      if (isspace(ch)) { if (t.length()) return t; continue; }
      t += (char)ch;
    }
    return t;
  };
  String magic = token();
  if (magic != "P4") { err = "only binary PBM (P4) is supported"; return false; }
  int w = token().toInt(), h = token().toInt();
  if (w < 1 || w > tdj::kMaxWidth || h < 1 || h > tdj::kMaxHeight) {
    err = "PBM must be 1-" + String(tdj::kMaxWidth) + " wide and 1-128 tall (height = across the tape)";
    return false;
  }
  const size_t stride = (w + 7) / 8;
  const size_t bytes = stride * h;
  if (bytes > 96 * 1024) { err = "PBM too large"; return false; }
  uint8_t* img = (uint8_t*)malloc(bytes);
  if (!img) { err = "out of memory"; return false; }
  if (in.read(img, bytes) != bytes) { free(img); err = "PBM truncated"; return false; }
  in.close();

  tdj::Header hd;
  hd.pages = 1;
  hd.copies = copies < 1 ? 1 : (copies > 99 ? 99 : copies);
  strlcpy(hd.title, title.length() ? title.c_str() : "image", sizeof hd.title);
  File f = LittleFS.open(out_path, "w");
  if (!f) { free(img); err = "cannot create job file"; return false; }
  uint8_t hb[tdj::kHeaderSize];
  tdj::write_header(hd, hb);
  f.write(hb, sizeof hb);
  uint8_t ph[4];
  tdj::write_page_header(w, h, ph);
  f.write(ph, 4);
  const int cb = (h + 7) / 8;
  uint8_t col[16];
  bool ok = true;
  for (int x = 0; x < w && ok; ++x) {
    memset(col, 0, sizeof col);
    for (int y = 0; y < h; ++y)
      if (img[y * stride + (x >> 3)] & (0x80 >> (x & 7))) col[y >> 3] |= 0x80 >> (y & 7);
    ok = f.write(col, cb) == (size_t)cb;
  }
  f.close();
  free(img);
  if (!ok) { err = "flash write failed"; LittleFS.remove(out_path); }
  return ok;
}

}  // namespace render
