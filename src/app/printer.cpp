#include "printer.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <stdarg.h>
#include <vector>
#include "jobs.h"
#include "printer_usb.h"
#include "settings.h"
#include "events.h"

Printer printer;

struct PLock {
  SemaphoreHandle_t m;
  explicit PLock(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTake(m, portMAX_DELAY); }
  ~PLock() { xSemaphoreGive(m); }
};

bool Printer::begin(int cs, int intr) {
  mu_ = xSemaphoreCreateMutex();
  q_ = xQueueCreate(8, sizeof(uint32_t));
  snap_.cs = cs; snap_.intr = intr;
  module_ok_ = cs >= 0 && intr >= 0 && printerUsb.begin(cs, intr);
  snap_.state = module_ok_ ? PState::NO_PRINTER : PState::NO_MODULE;
  // Same core as the MAX3421E interrupt task (Arduino core 1); above loop().
  xTaskCreatePinnedToCore(task_fn, "printer", 8192, this, 3, nullptr, 1);
  return module_ok_;
}

PrinterSnapshot Printer::snapshot() {
  PLock l(mu_);
  PrinterSnapshot s = snap_;
  s.status_age_ms = status_ms_ ? millis() - status_ms_ : 0;
  s.queued = uxQueueMessagesWaiting(q_);
  return s;
}

uint32_t Printer::seq() { PLock l(mu_); return snap_.seq; }
void Printer::bump() { snap_.seq++; }

bool Printer::enqueue(uint32_t id, String& err) {
  {
    PLock l(mu_);
    if (snap_.state == PState::NO_MODULE) { err = "USB Module not found"; return false; }
    if (snap_.state == PState::NO_PRINTER) { err = "no printer attached"; return false; }
  }
  if (xQueueSend(q_, &id, 0) != pdTRUE) { err = "print queue full"; return false; }
  PLock l(mu_);
  bump();
  return true;
}

void Printer::set_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PLock l(mu_);
  vsnprintf(snap_.last_error, sizeof snap_.last_error, fmt, ap);
  va_end(ap);
  bump();
}

void Printer::update_live_error() {
  // caller holds mu_
  if (snap_.status_valid) pt::describe_errors(snap_.st, snap_.printer_error, sizeof snap_.printer_error);
  else snap_.printer_error[0] = 0;
}

static void color_key(uint8_t w, uint8_t m, char* k) { snprintf(k, 8, "c%02x%02x", w, m); }

void Printer::load_colors_locked() {
  // caller holds mu_; sig_w_/sig_m_ describe the loaded cassette
  Preferences p;
  uint16_t v = 0;
  char k[8];
  color_key(sig_w_, sig_m_, k);
  if (p.begin("tapecol", true)) { v = p.getUShort(k, 0); p.end(); }
  if (v) {
    snap_.tape_color = v >> 8;
    snap_.ink_color = v & 0xFF;
    snap_.colors_user = true;
  } else {
    snap_.tape_color = snap_.st.tape_color();
    snap_.ink_color = snap_.st.text_color();
    snap_.colors_user = false;
  }
}

// After every new status block: notice cassette changes (width/media change,
// or the cover opened and closed again) and pick the colours to show.
void Printer::on_status_locked() {
  const pt::Status& s = snap_.st;
  const bool open = s.err2() & 0x10;
  const bool first = sig_w_ == 0xFF;
  bool changed = s.width_mm() != sig_w_ || s.media_type() != sig_m_;
  if (open) cover_open_ = true;
  if (!open && cover_open_) { cover_open_ = false; changed = true; }
  if (!changed) return;
  sig_w_ = s.width_mm();
  sig_m_ = s.media_type();
  load_colors_locked();
  if (!first) {
    snap_.tape_epoch++;
    snap_.colors_confirmed = !s.width_mm();  // nothing to confirm with no tape
    events.add("Tape changed: %u mm %s, colours %s", s.width_mm(), pt::media_type_name(s.media_type()),
               snap_.colors_user ? "remembered (check them)" : "unknown");
  } else {
    // At boot assume the cassette is the one last confirmed for this type.
    snap_.colors_confirmed = snap_.colors_user || !s.width_mm();
  }
  bump();
}

bool Printer::set_colors(uint8_t tape, uint8_t ink, String& err) {
  PLock l(mu_);
  if (!snap_.status_valid || !snap_.st.width_mm()) { err = "no tape loaded"; return false; }
  char k[8];
  color_key(sig_w_, sig_m_, k);
  Preferences p;
  if (!p.begin("tapecol", false)) { err = "settings storage failed"; return false; }
  p.putUShort(k, (uint16_t)(tape << 8 | ink));
  p.end();
  snap_.tape_color = tape;
  snap_.ink_color = ink;
  snap_.colors_user = true;
  snap_.colors_confirmed = true;
  bump();
  return true;
}

bool Printer::read_status(pt::Status& s, uint32_t timeout_ms) {
  uint8_t buf[64];
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    int n = printerUsb.read(buf, sizeof buf, timeout_ms - (millis() - t0));
    if (n < 0) return false;
    if (n >= 32 && s.parse(buf, n)) return true;
  }
  return false;
}

bool Printer::query_status(uint32_t timeout_ms) {
  uint8_t req[110];
  size_t n = pt::build_status_request(req, sizeof req);
  pt::Status s;
  bool ok = printerUsb.write(req, n, 2000) && read_status(s, timeout_ms);
  PLock l(mu_);
  if (ok) {
    bool changed = !snap_.status_valid || memcmp(snap_.st.raw, s.raw, 32) != 0;
    snap_.st = s;
    snap_.status_valid = true;
    status_ms_ = millis();
    update_live_error();
    on_status_locked();
    if (snap_.state != PState::PRINTING) snap_.state = s.has_error() ? PState::ERROR : PState::IDLE;
    if (changed) bump();
  }
  return ok;
}

void Printer::task_fn(void* p) { ((Printer*)p)->run(); }

void Printer::run() {
  uint32_t gen = 0;
  uint32_t last_poll = 0;
  for (;;) {
    if (!module_ok_) { vTaskDelay(1000); continue; }
    printerUsb.pump(2);
    if (printerUsb.generation() != gen) {
      gen = printerUsb.generation();
      PLock l(mu_);
      if (printerUsb.ready()) {
        strlcpy(snap_.product, printerUsb.product(), sizeof snap_.product);
        strlcpy(snap_.serial, printerUsb.serial(), sizeof snap_.serial);
        snap_.state = PState::IDLE;
        want_status_ = true;
        events.add("Printer attached: %s %s (%04X:%04X)", printerUsb.manufacturer(), printerUsb.product(),
                   printerUsb.vid(), printerUsb.pid());
      } else {
        if (snap_.state != PState::NO_PRINTER)
          events.add(printerUsb.attached() ? "USB device %04X:%04X is not a printer" : "Printer detached",
                     printerUsb.vid(), printerUsb.pid());
        snap_.state = PState::NO_PRINTER;
        snap_.status_valid = false;
        snap_.product[0] = 0;
        snap_.label_held = false;
        sig_w_ = sig_m_ = 0xFF;  // next attach: treat like a boot
        cover_open_ = false;
        update_live_error();
      }
      bump();
    }
    if (!printerUsb.ready()) {
      // drop anything queued while there is no printer
      uint32_t id;
      while (xQueueReceive(q_, &id, 0) == pdTRUE) set_error("Job %lu dropped: no printer", (unsigned long)id);
      continue;
    }
    uint32_t id;
    if (xQueueReceive(q_, &id, 0) == pdTRUE) {
      print_job(id);
      last_poll = millis();
      continue;
    }
    // Idle: poll the status every few seconds (tape changes, cover open).
    if (want_status_ || millis() - last_poll > 4000) {
      want_status_ = false;
      last_poll = millis();
      query_status();
    }
  }
}

// A page streamed from the job file a column at a time.
struct FilePage {
  uint32_t offset;   // first column byte
  uint16_t w, h;
};

void Printer::print_job(uint32_t id) {
  JobEntry e;
  if (!jobs.find(id, e)) { set_error("Job %lu not found", (unsigned long)id); return; }
  const uint32_t copies = e.hdr.copies;
  {
    PLock l(mu_);
    snap_.state = PState::PRINTING;
    snap_.job_id = id;
    strlcpy(snap_.job_title, e.hdr.title, sizeof snap_.job_title);
    snap_.pages_done = 0;
    snap_.pages_total = e.hdr.pages * copies;
    snap_.last_error[0] = 0;
    bump();
  }
  auto finish = [&](bool ok) {
    PLock l(mu_);
    snap_.state = (snap_.status_valid && snap_.st.has_error()) ? PState::ERROR : PState::IDLE;
    snap_.last_job_id = id;
    snap_.last_ok = ok;
    snap_.job_id = 0;
    if (ok) { snap_.jobs_done++; snap_.labels_done += snap_.pages_total; }
    bump();
  };

  // Fresh status: tape present, no errors, geometry known.
  if (!query_status(2000)) { set_error("Printer did not answer the status request"); finish(false); return; }
  PrinterSnapshot s = snapshot();
  if (s.st.has_error()) { set_error("Printer error: %s", s.printer_error); finish(false); return; }
  const pt::TapeGeom* g = pt::tape_geom(s.st.width_mm());
  if (!g) { set_error("Unsupported or no tape (%u mm)", s.st.width_mm()); finish(false); return; }
  if (e.max_height > g->print_pins) {
    set_error("Job is %u dots tall (%u mm tape); the %u mm tape loaded prints %u", e.max_height, e.hdr.tape_mm,
              g->width_mm, g->print_pins);
    finish(false);
    return;
  }

  File f = LittleFS.open(JobStore::path(id), "r");
  if (!f) { set_error("Cannot open job %lu", (unsigned long)id); finish(false); return; }
  std::vector<FilePage> fp;
  uint32_t off = tdj::kHeaderSize;
  for (int i = 0; i < e.hdr.pages; ++i) {
    uint8_t ph[4];
    tdj::PageInfo pi;
    f.seek(off);
    f.read(ph, 4);
    tdj::read_page_header(ph, pi);
    fp.push_back({off + 4, pi.width, pi.height});
    off += 4 + pi.bytes();
  }
  // Pages for every copy; column readers share the open file.
  const bool mirror = e.hdr.flags & tdj::F_MIRROR;
  std::vector<pt::Page> pages;
  pages.reserve(fp.size() * copies);
  int cur_page = -1;
  for (uint32_t c = 0; c < copies; ++c) {
    for (size_t i = 0; i < fp.size(); ++i) {
      pt::Page p;
      p.width = fp[i].w; p.height = fp[i].h;
      const FilePage fpi = fp[i];
      const int idx = (int)(c * fp.size() + i);
      p.column = [&f, fpi, idx, mirror, &cur_page](uint32_t x, uint8_t* out) -> bool {
        size_t cb = pt::Page::column_bytes(fpi.h);
        // sequential reads within a page; seek on page change (or always when mirrored)
        if (mirror || cur_page != idx || x == 0) {
          if (!f.seek(fpi.offset + x * cb)) return false;
          cur_page = idx;
        }
        return f.read(out, cb) == cb;
      };
      pages.push_back(p);
    }
  }

  pt::JobOptions opt;
  opt.auto_cut = e.hdr.flags & tdj::F_AUTOCUT;
  opt.chain = (e.hdr.flags & tdj::F_CHAIN) || settings.chain;
  opt.mirror = mirror;
  uint32_t margin = e.hdr.margin_dots;
  uint32_t min_margin = (uint32_t)settings.margin_mm10 * pt::kDpi / 254;
  if (margin < min_margin) margin = min_margin;
  opt.margin_dots = margin;

  // Stream to the printer in 512-byte pieces. The printer NAKs while it is
  // busy printing earlier pages, so a piece can take a whole page time.
  static uint8_t out[512];
  size_t used = 0;
  int pages_done = 0;
  bool fail = false;
  auto drain_status = [&](uint32_t wait) {
    pt::Status st;
    while (read_status(st, wait)) {
      wait = 5;
      PLock l(mu_);
      snap_.st = st;
      snap_.status_valid = true;
      status_ms_ = millis();
      update_live_error();
      on_status_locked();
      if (st.status_type() == pt::ST_PRINT_DONE) { ++pages_done; snap_.pages_done = pages_done; }
      if (st.status_type() == pt::ST_ERROR || st.has_error()) { fail = true; }
      bump();
      if (fail) break;
    }
  };
  auto sink = [&](const uint8_t* p, size_t n) -> bool {
    while (n) {
      size_t k = sizeof out - used < n ? sizeof out - used : n;
      memcpy(out + used, p, k); used += k; p += k; n -= k;
      if (used == sizeof out) {
        if (!printerUsb.write(out, used, 30000)) return false;
        used = 0;
        drain_status(3);
        if (fail) return false;
      }
    }
    return true;
  };
  uint32_t t0 = millis();
  bool ok = pt::encode_job(*g, s.st.media_type(), pages.data(), (int)pages.size(), opt, sink);
  if (ok && used) ok = printerUsb.write(out, used, 30000);
  f.close();
  if (!ok || fail) {
    PrinterSnapshot s2 = snapshot();
    if (fail) set_error("Printer error during job: %s", s2.printer_error[0] ? s2.printer_error : "unknown");
    else set_error("USB transfer to the printer failed");
    // A half-sent job leaves the printer waiting; reset it.
    uint8_t rst[102] = {0};
    rst[100] = 0x1B; rst[101] = '@';
    printerUsb.write(rst, sizeof rst, 2000);
    events.add("Job %lu failed: %s", (unsigned long)id, snapshot().last_error);
    finish(false);
    return;
  }
  // Wait for every page to report "printing completed".
  const int total = (int)pages.size();
  uint32_t last_progress = millis();
  int seen = pages_done;
  while (pages_done < total && !fail && millis() - last_progress < 30000 && printerUsb.ready()) {
    drain_status(500);
    if (pages_done != seen) { seen = pages_done; last_progress = millis(); }
  }
  if (fail || pages_done < total) {
    PrinterSnapshot s2 = snapshot();
    if (fail) set_error("Printer error: %s", s2.printer_error[0] ? s2.printer_error : "unknown");
    else set_error("Printer stopped after %d of %d labels", pages_done, total);
    events.add("Job %lu failed: %s", (unsigned long)id, snapshot().last_error);
    finish(false);
    return;
  }
  jobs.mark_printed(id);
  {
    PLock l(mu_);
    snap_.label_held = opt.chain;
  }
  events.add("Printed job %lu \"%s\": %d label%s in %.1f s", (unsigned long)id, e.hdr.title, total,
             total == 1 ? "" : "s", (millis() - t0) / 1000.0f);
  finish(true);
}
