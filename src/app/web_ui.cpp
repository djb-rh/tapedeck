#include "web_ui.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <uri/UriBraces.h>
#include "app.h"
#include "jobs.h"
#include "printer.h"
#include "render.h"
#include "web_assets.h"

WebUi webUi;
static WebServer server(80);
static DNSServer dns;
static bool dns_running = false;
static const char* kUpload = "/spool/up.bin";
static const char* kJob = "/spool/job.tdj";
static const size_t kMaxUpload = 2 * 1024 * 1024;
static File g_up;
static size_t g_up_size = 0;
static bool g_up_overflow = false;

// ------------------------------------------------------------- helpers

static void send_json(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", out);
}

static void send_err(int code, const String& msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  send_json(code, d);
}

static void send_asset(const char* type, const uint8_t* gz, size_t len) {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, type, (const char*)gz, len);
}

// Settings and destructive actions: web password (HTTP basic, user "admin").
static bool admin_ok() {
  if (settings.web_pass.isEmpty()) return true;
  if (server.authenticate("admin", settings.web_pass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

// Printing: API key if one is set (header X-API-Key or ?key=), or the admin password.
static bool print_ok() {
  app.last_web_ms = millis();
  if (settings.api_key.isEmpty()) return true;
  if (server.header("X-API-Key") == settings.api_key || server.arg("key") == settings.api_key) return true;
  if (!settings.web_pass.isEmpty() && server.authenticate("admin", settings.web_pass.c_str())) return true;
  send_err(401, "API key required (X-API-Key header)");
  return false;
}

// Tape colours as CSS, so the editor and API clients can draw a true preview.
static const char* tape_css(uint8_t c) {
  switch (c) {
    case 0x01: case 0x20: case 0x70: case 0x90: return "#fbfbf6";
    case 0x03: case 0x09: case 0x21: return "#e8eef0";
    case 0x04: case 0x31: return "#d8262c";
    case 0x05: case 0x30: return "#1f5fb8";
    case 0x06: case 0x60: case 0x91: return "#f7d417";
    case 0x07: return "#2f9a4a";
    case 0x08: return "#1a1a1a";
    case 0x22: case 0x24: return "#c9cccf";
    case 0x23: return "#cfae5a";
    case 0x40: return "#ff7a1a";
    case 0x41: return "#e6ff3a";
    case 0x50: case 0x61: return "#f06aa7";
    case 0x51: return "#c4c4c4";
    case 0x52: return "#9fdc4a";
    case 0x62: return "#4fa6ff";
    default: return "#f4f4f0";
  }
}
static const char* ink_css(uint8_t c) {
  switch (c) {
    case 0x01: return "#ffffff";
    case 0x04: return "#d8262c";
    case 0x05: case 0x62: return "#1f5fb8";
    case 0x0A: return "#cfae5a";
    default: return "#111111";
  }
}

static const char* state_name(PState s) {
  switch (s) {
    case PState::NO_MODULE: return "no_module";
    case PState::NO_PRINTER: return "no_printer";
    case PState::IDLE: return "ready";
    case PState::PRINTING: return "printing";
    case PState::ERROR: return "error";
  }
  return "?";
}

static void fill_job(JsonObject o, const JobEntry& e) {
  o["id"] = e.id;
  o["title"] = String(e.hdr.title);  // copy: callers reuse the entry
  o["labels"] = e.hdr.pages;
  o["copies"] = e.hdr.copies;
  o["tape_mm"] = e.hdr.tape_mm;
  o["height"] = e.max_height;
  o["length_mm"] = roundf(e.total_cols * 25.4f / pt::kDpi * 10) / 10;
  o["created"] = e.hdr.created;
  o["source"] = String(e.source);
  o["printed"] = e.printed;
  o["bytes"] = e.file_size;
}

static void handle_status() {
  PrinterSnapshot s = printer.snapshot();
  JsonDocument d;
  d["state"] = state_name(s.state);
  JsonObject p = d["printer"].to<JsonObject>();
  p["product"] = s.product;
  p["serial"] = s.serial;
  p["error"] = s.printer_error;
  p["label_held"] = s.label_held;
  if (s.status_valid) {
    const pt::Status& st = s.st;
    JsonObject t = d["tape"].to<JsonObject>();
    t["width_mm"] = st.width_mm();
    const pt::TapeGeom* g = pt::tape_geom(st.width_mm());
    t["print_dots"] = g ? g->print_pins : 0;
    t["dpi"] = pt::kDpi;
    t["media"] = pt::media_type_name(st.media_type());
    t["color"] = pt::tape_color_name(s.tape_color);
    t["ink"] = pt::text_color_name(s.ink_color);
    t["color_css"] = tape_css(s.tape_color);
    t["ink_css"] = ink_css(s.ink_color);
    t["color_code"] = s.tape_color;
    t["ink_code"] = s.ink_color;
    // The PT-D600 always reports white/black; colours are the user's choice per cassette type.
    t["colors_from"] = s.colors_user ? "user" : "printer";
    t["colors_confirmed"] = s.colors_confirmed;
    t["epoch"] = s.tape_epoch;
    char raw[65];
    for (int i = 0; i < 32; ++i) snprintf(raw + i * 2, 3, "%02x", st.raw[i]);
    p["status_raw"] = raw;
    p["status_age_ms"] = s.status_age_ms;
  }
  JsonObject j = d["job"].to<JsonObject>();
  j["id"] = s.job_id;
  j["title"] = String(s.job_title);
  j["done"] = s.pages_done;
  j["total"] = s.pages_total;
  j["queued"] = s.queued;
  j["last_id"] = s.last_job_id;
  j["last_ok"] = s.last_ok;
  j["last_error"] = s.last_error;
  j["jobs_done"] = s.jobs_done;
  j["labels_done"] = s.labels_done;
  JsonObject dv = d["device"].to<JsonObject>();
  dv["version"] = TAPEDECK_VERSION;
  dv["hostname"] = settings.hostname;
  dv["ip"] = app_ip();
  dv["ap_mode"] = app.ap_mode;
  dv["ap_ssid"] = app.ap_ssid;
  dv["ssid"] = app.sta_connected ? WiFi.SSID() : settings.wifi_ssid;
  dv["rssi"] = app.sta_connected ? WiFi.RSSI() : 0;
  dv["uptime"] = (millis() - app.boot_ms) / 1000;
  dv["heap"] = ESP.getFreeHeap();
  dv["fs_used"] = LittleFS.usedBytes();
  dv["fs_total"] = LittleFS.totalBytes();
  dv["usb_cs"] = s.cs;
  dv["usb_int"] = s.intr;
  dv["api_key"] = !settings.api_key.isEmpty();
  dv["chain"] = settings.chain;
  dv["margin_mm"] = settings.margin_mm10 / 10.0f;
  dv["history_max"] = settings.history_max;
  send_json(200, d);
}

// GET /api/tape: recent cassettes (this width + type first) and colour presets. POST /api/tape (tape=CODE&ink=CODE, or
// preset=INDEX): set the loaded cassette's colours.
static void handle_tape_get() {
  JsonDocument d;
  PrinterSnapshot s = printer.snapshot();
  TapeChoice rec[kRecentMax];
  int nr = printer.recent(rec, kRecentMax);
  JsonArray ra = d["recent"].to<JsonArray>();
  for (int i = 0; i < nr; ++i) {
    JsonObject o = ra.add<JsonObject>();
    int p = pt::find_color_preset(rec[i].tape, rec[i].ink);
    o["name"] = p >= 0 ? String(pt::kColorPresets[p].name)
                       : String(pt::text_color_name(rec[i].ink)) + " on " + pt::tape_color_name(rec[i].tape);
    o["tape"] = rec[i].tape;
    o["ink"] = rec[i].ink;
    o["tape_css"] = tape_css(rec[i].tape);
    o["ink_css"] = ink_css(rec[i].ink);
    o["width_mm"] = rec[i].w;
    o["media"] = pt::media_type_name(rec[i].m);
    o["media_code"] = rec[i].m;
    o["same_type"] = s.status_valid && rec[i].w == s.st.width_mm() && rec[i].m == s.st.media_type();
  }
  JsonArray a = d["presets"].to<JsonArray>();
  for (int i = 0; i < pt::kColorPresetCount; ++i) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = pt::kColorPresets[i].name;
    o["tape"] = pt::kColorPresets[i].tape;
    o["ink"] = pt::kColorPresets[i].ink;
    o["tape_css"] = tape_css(pt::kColorPresets[i].tape);
    o["ink_css"] = ink_css(pt::kColorPresets[i].ink);
  }
  send_json(200, d);
}

static void handle_tape_post() {
  if (!print_ok()) return;
  int tape = -1, ink = -1;
  if (server.hasArg("preset")) {
    int i = server.arg("preset").toInt();
    if (i >= 0 && i < pt::kColorPresetCount) { tape = pt::kColorPresets[i].tape; ink = pt::kColorPresets[i].ink; }
  } else if (server.hasArg("tape") && server.hasArg("ink")) {
    tape = server.arg("tape").toInt();
    ink = server.arg("ink").toInt();
  }
  if (tape < 1 || tape > 255 || ink < 1 || ink > 255) { send_err(400, "give preset=N, or tape=CODE&ink=CODE"); return; }
  String err;
  if (!printer.set_colors(tape, ink, err)) { send_err(409, err); return; }
  events.add("Tape colours set: %s on %s", pt::text_color_name(ink), pt::tape_color_name(tape));
  JsonDocument d;
  d["ok"] = true;
  d["color"] = pt::tape_color_name(tape);
  d["ink"] = pt::text_color_name(ink);
  send_json(200, d);
}

// POST /api/tape/forget: tape=CODE&ink=CODE&width_mm=W&media_code=M
static void handle_tape_forget() {
  if (!print_ok()) return;
  TapeChoice t = {(uint8_t)server.arg("width_mm").toInt(), (uint8_t)server.arg("media_code").toInt(),
                  (uint8_t)server.arg("tape").toInt(), (uint8_t)server.arg("ink").toInt()};
  if (!printer.forget(t)) { send_err(404, "not in the recent list"); return; }
  JsonDocument d;
  d["ok"] = true;
  send_json(200, d);
}

static void handle_jobs() {
  JsonDocument d;
  JsonArray a = d["jobs"].to<JsonArray>();
  JobEntry e;
  for (int i = 0; jobs.at(i, e); ++i) fill_job(a.add<JsonObject>(), e);
  send_json(200, d);
}

// Options for /api/print. The WebServer does not parse the query string of a
// raw (non-form) POST, so binary uploads carry them as headers: X-Print,
// X-Title, X-Copies, X-Tape-MM, X-Source. Query args still work for GETs.
static String opt(const char* name) {
  String h = String("X-") + name;
  if (server.hasHeader(h)) return server.header(h);
  String q = String(name);
  q.toLowerCase();
  q.replace("-", "_");
  return server.arg(q);
}

static uint32_t path_id() { return strtoul(server.pathArg(0).c_str(), nullptr, 10); }

static void handle_job_get() {
  uint32_t id = path_id();
  JobEntry e;
  if (!jobs.find(id, e)) { send_err(404, "no such job"); return; }
  File f = LittleFS.open(JobStore::path(id), "r");
  if (!f) { send_err(404, "job file missing"); return; }
  server.sendHeader("Cache-Control", "max-age=86400");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.streamFile(f, "application/x-tapedeck-job");
  f.close();
}

static void queue_reply(uint32_t id, bool print) {
  JsonDocument d;
  JobEntry e;
  jobs.find(id, e);
  d["ok"] = true;
  fill_job(d["job"].to<JsonObject>(), e);
  if (print) {
    String err;
    if (!printer.enqueue(id, err)) { d["ok"] = false; d["error"] = err; d["queued"] = false; send_json(503, d); return; }
    d["queued"] = true;
  } else {
    d["queued"] = false;
  }
  send_json(200, d);
}

static void handle_job_print() {
  if (!print_ok()) return;
  uint32_t id = path_id();
  JobEntry e;
  if (!jobs.find(id, e)) { send_err(404, "no such job"); return; }
  queue_reply(id, true);
}

static void handle_job_delete() {
  if (!print_ok()) return;
  if (!jobs.remove(path_id())) { send_err(404, "no such job"); return; }
  JsonDocument d;
  d["ok"] = true;
  send_json(200, d);
}

// POST /api/print: body is a TDJ1 job, a P4 PBM, or a JSON text job.
static void handle_print_raw() {
  HTTPRaw& r = server.raw();
  if (r.status == RAW_START) {
    g_up_size = 0;
    g_up_overflow = false;
    LittleFS.remove(kUpload);
    g_up = LittleFS.open(kUpload, "w");
  } else if (r.status == RAW_WRITE) {
    if (g_up && !g_up_overflow) {
      if (g_up_size + r.currentSize > kMaxUpload) g_up_overflow = true;
      else if (g_up.write(r.buf, r.currentSize) == r.currentSize) g_up_size += r.currentSize;
      else g_up_overflow = true;
    }
  } else if (r.status == RAW_END || r.status == RAW_ABORTED) {
    if (g_up) g_up.close();
  }
}

static void handle_print_done() {
  if (!print_ok()) return;
  if (g_up_overflow) { send_err(413, "upload too large or flash full"); return; }
  if (!g_up_size) { send_err(400, "empty body"); return; }
  File f = LittleFS.open(kUpload, "r");
  uint8_t head[4] = {0};
  if (f) { f.read(head, 4); f.close(); }
  String po = opt("Print");
  po.toLowerCase();
  bool print = !(po == "0" || po == "false" || po == "no") && server.uri() != "/api/save";
  String err;
  String src;
  const char* job_path = kUpload;
  if (!memcmp(head, "TDJ1", 4)) {
    src = opt("Source").length() ? opt("Source") : "api";
  } else if (head[0] == 'P' && head[1] == '4') {
    int copies = opt("Copies").length() ? opt("Copies").toInt() : 1;
    if (!render::pbm_job(kUpload, kJob, opt("Title"), copies, err)) { send_err(400, err); return; }
    LittleFS.remove(kUpload);
    job_path = kJob;
    src = "pbm";
  } else {
    // JSON text job: needs the loaded tape's height.
    if (g_up_size > 32 * 1024) { send_err(413, "JSON job too large"); return; }
    PrinterSnapshot s = printer.snapshot();
    int h = 0, mm = 0;
    if (s.status_valid) {
      const pt::TapeGeom* g = pt::tape_geom(s.st.width_mm());
      if (g) { h = g->print_pins; mm = g->width_mm; }
    }
    // Clients may ask for a tape width explicitly (preview without a printer).
    if (opt("Tape-MM").length()) {
      const pt::TapeGeom* g = pt::tape_geom(opt("Tape-MM").toInt());
      if (g) { h = g->print_pins; mm = g->width_mm; }
    }
    File jf = LittleFS.open(kUpload, "r");
    String body = jf.readString();
    jf.close();
    LittleFS.remove(kUpload);
    if (!render::text_job(body.c_str(), body.length(), h, mm, kJob, err)) { send_err(400, err); return; }
    if (body.indexOf("\"print\"") >= 0) {  // {"print": false} in the body
      JsonDocument pd;
      JsonDocument filter;
      filter["print"] = true;
      if (!deserializeJson(pd, body, DeserializationOption::Filter(filter)) && pd["print"].is<bool>()) print = print && pd["print"].as<bool>();
    }
    job_path = kJob;
    src = "text";
  }
  uint32_t id = jobs.add(job_path, src.c_str(), err);
  if (!id) { send_err(400, err); return; }
  queue_reply(id, print);
}

// ------------------------------------------------------------- settings

static void handle_config_get() {
  if (!admin_ok()) return;
  JsonDocument d;
  d["ssid"] = settings.wifi_ssid;
  d["hostname"] = settings.hostname;
  d["has_pass"] = !settings.wifi_pass.isEmpty();
  d["has_web_pass"] = !settings.web_pass.isEmpty();
  d["has_api_key"] = !settings.api_key.isEmpty();
  d["tz"] = settings.tz;
  d["ntp"] = settings.ntp;
  d["margin_mm"] = settings.margin_mm10 / 10.0f;
  d["chain"] = settings.chain;
  d["history_max"] = settings.history_max;
  d["disp_timeout_s"] = settings.disp_timeout_s;
  d["brightness"] = settings.brightness;
  send_json(200, d);
}

static void handle_config_post() {
  if (!admin_ok()) return;
  bool wifi_changed = false, host_changed = false;
  auto has = [](const char* k) { return server.hasArg(k); };
  if (has("ssid") && server.arg("ssid") != settings.wifi_ssid) { settings.wifi_ssid = server.arg("ssid"); wifi_changed = true; }
  if (has("pass") && server.arg("pass").length()) { settings.wifi_pass = server.arg("pass"); wifi_changed = true; }
  if (has("clear_pass")) { settings.wifi_pass = ""; wifi_changed = true; }
  if (has("hostname")) {
    String h = server.arg("hostname");
    h.trim();
    h.toLowerCase();
    if (h.length() && h != settings.hostname) { settings.hostname = h; host_changed = true; }
  }
  if (has("web_pass")) settings.web_pass = server.arg("web_pass");
  if (has("api_key")) settings.api_key = server.arg("api_key");
  if (has("tz")) settings.tz = server.arg("tz");
  if (has("ntp")) settings.ntp = server.arg("ntp");
  if (has("margin_mm")) { int m = (int)(server.arg("margin_mm").toFloat() * 10 + 0.5f); settings.margin_mm10 = m < 20 ? 20 : (m > 250 ? 250 : m); }
  if (has("chain")) settings.chain = server.arg("chain") == "1" || server.arg("chain") == "true";
  if (has("history_max")) { int n = server.arg("history_max").toInt(); settings.history_max = n < 1 ? 1 : (n > 50 ? 50 : n); }
  if (has("disp_timeout_s")) settings.disp_timeout_s = server.arg("disp_timeout_s").toInt();
  if (has("brightness")) settings.brightness = constrain(server.arg("brightness").toInt(), 10, 255);
  settings.save();
  events.add("Settings saved%s", wifi_changed || host_changed ? ", reconnecting" : "");
  JsonDocument d;
  d["ok"] = true;
  d["reconnect"] = wifi_changed || host_changed;
  send_json(200, d);
  if (wifi_changed || host_changed) app_wifi_reconfigure();
}

static void handle_scan() {
  if (!admin_ok()) return;
  int n = WiFi.scanNetworks(false, false);
  JsonDocument d;
  JsonArray a = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; ++i) {
    bool dup = false;
    for (JsonObject o : a) if (o["ssid"] == WiFi.SSID(i)) { dup = true; break; }
    if (dup || WiFi.SSID(i).isEmpty()) continue;
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  send_json(200, d);
}

static void handle_update_done() {
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", ok ? "Update OK, rebooting" : "Update FAILED");
  events.add("Firmware update %s", ok ? "applied, rebooting" : "failed");
  if (ok) app_request_reboot(800);
}

static void handle_update_upload() {
  if (!settings.web_pass.isEmpty() && !server.authenticate("admin", settings.web_pass.c_str())) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  }
}

static bool captive_redirect() {
  if (!webUi.captive()) return false;
  String host = server.hostHeader();
  String ip = WiFi.softAPIP().toString();
  if (host == ip || host == settings.hostname + ".local") return false;
  server.sendHeader("Location", "http://" + ip + "/setup", true);
  server.send(302, "text/plain", "");
  return true;
}

void WebUi::begin() {
  const char* hdrs[] = {"X-API-Key", "X-Print", "X-Title", "X-Copies", "X-Tape-MM", "X-Source"};
  server.collectHeaders(hdrs, 6);
  server.on("/", HTTP_GET, []() { if (captive_redirect()) return; send_asset("text/html", WEB_INDEX_HTML, WEB_INDEX_HTML_LEN); });
  server.on("/setup", HTTP_GET, []() { send_asset("text/html", WEB_SETUP_HTML, WEB_SETUP_HTML_LEN); });
  server.on("/api/status", HTTP_GET, handle_status);
  server.on("/api/jobs", HTTP_GET, handle_jobs);
  server.on("/api/tape", HTTP_GET, handle_tape_get);
  server.on("/api/tape", HTTP_POST, handle_tape_post);
  server.on("/api/tape/forget", HTTP_POST, handle_tape_forget);
  server.on(UriBraces("/api/jobs/{}"), HTTP_GET, handle_job_get);
  server.on(UriBraces("/api/jobs/{}"), HTTP_DELETE, handle_job_delete);
  server.on(UriBraces("/api/jobs/{}/print"), HTTP_POST, handle_job_print);
  server.on("/api/print", HTTP_POST, handle_print_done, handle_print_raw);
  server.on("/api/save", HTTP_POST, handle_print_done, handle_print_raw);
  server.on("/api/print", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-API-Key, X-Print, X-Title, X-Copies, X-Tape-MM, X-Source");
    server.send(204);
  });
  server.on("/api/events", HTTP_GET, []() {
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < events.count(); ++i) {
      const Event& e = events.newest(i);
      a.add(EventLog::format_time(e) + "  " + e.msg);
    }
    send_json(200, d);
  });
  server.on("/api/config", HTTP_GET, handle_config_get);
  server.on("/api/config", HTTP_POST, handle_config_post);
  server.on("/api/scan", HTTP_GET, handle_scan);
  server.on("/api/refresh", HTTP_POST, []() { printer.refresh_status(); server.send(200, "text/plain", "ok"); });
  server.on("/api/reboot", HTTP_POST, []() {
    if (!admin_ok()) return;
    server.send(200, "text/plain", "rebooting");
    app_request_reboot(500);
  });
  server.on("/api/factory", HTTP_POST, []() {
    if (!admin_ok()) return;
    server.send(200, "text/plain", "erasing");
    app_factory_reset();
  });
  server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  const char* probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                          "/ncsi.txt", "/connecttest.txt", "/fwlink", "/success.txt", "/canonical.html"};
  for (auto p : probes) server.on(p, HTTP_GET, []() { if (!captive_redirect()) server.send(200, "text/plain", "ok"); });
  server.onNotFound([]() { if (!captive_redirect()) server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("[web] listening on 80");
}

void WebUi::set_captive(bool on) {
  captive_ = on;
  if (on && !dns_running) { dns.setErrorReplyCode(DNSReplyCode::NoError); dns.start(53, "*", WiFi.softAPIP()); dns_running = true; }
  if (!on && dns_running) { dns.stop(); dns_running = false; }
}

void WebUi::task() {
  if (dns_running) dns.processNextRequest();
  server.handleClient();
}
