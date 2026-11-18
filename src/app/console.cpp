// Serial console (115200). Mostly for development and headless setup:
//   help | status | jobs | events
//   print ID | del ID | dump ID (job file as HEX: lines)
//   text {json}     render a JSON text job and print it   (render {json}: store only)
//   btn a|b|c [hold]  press a button      shot   screen dump (tools/screenshot.py)
//   wifi SSID [PASS]  join a network       host NAME   reboot
#include "console.h"
#include <LittleFS.h>
#include <WiFi.h>
#include "app.h"
#include "display.h"
#include "jobs.h"
#include "printer.h"
#include "render.h"
#include <HTTPClient.h>
#include <M5Unified.h>

Console console;

void Console::begin() { line_.reserve(256); }

// HTTP self-test: a separate task talks to our own web server over loopback
// while loop() serves it, exercising the same paths a browser or API client uses.
static void http_req(const char* method, const String& path, const uint8_t* body, size_t len, const char* ctype, const char* hk = nullptr, const char* hv = nullptr) {
  HTTPClient h;
  h.begin("http://127.0.0.1" + path);
  if (hk) h.addHeader(hk, hv);
  h.setTimeout(15000);
  if (ctype) h.addHeader("Content-Type", ctype);
  int code;
  if (!strcmp(method, "GET")) code = h.GET();
  else if (!strcmp(method, "DELETE")) code = h.sendRequest("DELETE");
  else code = h.sendRequest(method, (uint8_t*)body, len);
  String r = code > 0 ? h.getString() : HTTPClient::errorToString(code);
  h.end();
  if (r.length() > 300) r = r.substring(0, 300) + "...(" + String(r.length()) + " bytes)";
  Serial.printf("HTTP %s %s -> %d %s\r\n", method, path.c_str(), code, r.c_str());
}

static void http_test_task(void*) {
  http_req("GET", "/", nullptr, 0, nullptr);
  http_req("GET", "/setup", nullptr, 0, nullptr);
  http_req("GET", "/api/status", nullptr, 0, nullptr);
  const char* js = "{\"labels\":[\"HTTP test\\nline 2\",{\"text\":\"mono\",\"font\":\"mono\"}],\"title\":\"http json\"}";
  http_req("POST", "/api/save", (const uint8_t*)js, strlen(js), "application/json");
  // 16x24 PBM: a hollow box
  static uint8_t pbm[64 + 48];
  int n = snprintf((char*)pbm, 64, "P4\n# test\n16 24\n");
  for (int y = 0; y < 24; ++y) {
    uint16_t row = (y == 0 || y == 23) ? 0xFFFF : 0x8001;
    pbm[n + y * 2] = row >> 8; pbm[n + y * 2 + 1] = row & 0xFF;
  }
  http_req("POST", "/api/print", pbm, n + 48, "image/x-portable-bitmap", "X-Print", "0");
  {
    HTTPClient h; h.begin("http://127.0.0.1/api/save"); h.addHeader("X-Title", "box"); h.addHeader("X-Copies", "2");
    int code = h.sendRequest("POST", pbm, n + 48); Serial.printf("HTTP POST /api/save (X-Title box, X-Copies 2) -> %d %s\r\n", code, h.getString().c_str()); h.end();
  }
  const char* js2 = "{\"text\":\"no print\",\"print\":false}";
  http_req("POST", "/api/print", (const uint8_t*)js2, strlen(js2), "application/json");
  // TDJ1 straight through: copy the newest history job
  JobEntry e;
  if (jobs.at(0, e)) {
    File f = LittleFS.open(JobStore::path(e.id), "r");
    size_t sz = f.size();
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (buf) {
      f.read(buf, sz);
      http_req("POST", "/api/save", buf, sz, "application/octet-stream", "X-Source", "web");
      free(buf);
    }
    f.close();
  }
  http_req("POST", "/api/print", (const uint8_t*)"{bad", 4, "application/json");
  http_req("GET", "/api/jobs", nullptr, 0, nullptr);
  if (jobs.at(0, e)) {
    http_req("GET", "/api/jobs/" + String(e.id), nullptr, 0, nullptr);
    http_req("DELETE", "/api/jobs/" + String(e.id), nullptr, 0, nullptr);
  }
  http_req("GET", "/api/jobs/99999", nullptr, 0, nullptr);
  http_req("GET", "/api/config", nullptr, 0, nullptr);
  Serial.println("HTTPTEST DONE");
  vTaskDelete(nullptr);
}

static void cmd_text(const String& json, bool print) {
  PrinterSnapshot s = printer.snapshot();
  int h = 0, mm = 0;
  if (s.status_valid) {
    const pt::TapeGeom* g = pt::tape_geom(s.st.width_mm());
    if (g) { h = g->print_pins; mm = g->width_mm; }
  }
  if (!h) { h = 70; mm = 12; Serial.println("(no tape status; rendering for 12 mm)"); }
  String err;
  uint32_t t0 = millis();
  if (!render::text_job(json.c_str(), json.length(), h, mm, "/spool/job.tdj", err)) { Serial.printf("render failed: %s\r\n", err.c_str()); return; }
  uint32_t id = jobs.add("/spool/job.tdj", "serial", err);
  if (!id) { Serial.printf("add failed: %s\r\n", err.c_str()); return; }
  Serial.printf("job %lu rendered in %lu ms\r\n", (unsigned long)id, millis() - t0);
  if (print) {
    if (!printer.enqueue(id, err)) Serial.printf("not queued: %s\r\n", err.c_str());
    else Serial.println("queued");
  }
}

static void cmd_dump(uint32_t id) {
  File f = LittleFS.open(JobStore::path(id), "r");
  if (!f) { Serial.println("no such job"); return; }
  Serial.printf("DUMP %lu %u\r\n", (unsigned long)id, (unsigned)f.size());
  uint8_t b[48];
  while (int n = f.read(b, sizeof b)) {
    Serial.print("HEX:");
    for (int i = 0; i < n; ++i) Serial.printf("%02X", b[i]);
    Serial.println();
  }
  f.close();
  Serial.println("END");
}

void Console::run(String l) {
  l.trim();
  if (!l.length()) return;
  int sp = l.indexOf(' ');
  String cmd = sp < 0 ? l : l.substring(0, sp);
  String arg = sp < 0 ? "" : l.substring(sp + 1);
  arg.trim();
  if (cmd == "help") {
    Serial.println("status jobs events print ID del ID dump ID text {json} render {json} btn a|b|c [hold] shot wifi SSID [PASS] host NAME reboot");
  } else if (cmd == "status") {
    PrinterSnapshot s = printer.snapshot();
    Serial.printf("state %d, product '%s', ip %s, ap %d sta %d, heap %u, jobs %d\r\n", (int)s.state, s.product,
                  app_ip().c_str(), app.ap_mode, app.sta_connected, ESP.getFreeHeap(), jobs.count());
    if (s.status_valid)
      Serial.printf("tape %u mm %s on %s, err '%s', last job %lu ok %d '%s'\r\n", s.st.width_mm(),
                    pt::text_color_name(s.ink_color), pt::tape_color_name(s.tape_color), s.printer_error,
                    (unsigned long)s.last_job_id, s.last_ok, s.last_error);
  } else if (cmd == "jobs") {
    JobEntry e;
    for (int i = 0; jobs.at(i, e); ++i)
      Serial.printf("%lu  %u pages x%u  %lu cols  h%u  %s  '%s'\r\n", (unsigned long)e.id, e.hdr.pages, e.hdr.copies,
                    (unsigned long)e.total_cols, e.max_height, e.source, e.hdr.title);
  } else if (cmd == "events") {
    for (int i = events.count() - 1; i >= 0; --i) {
      const Event& e = events.newest(i);
      Serial.printf("%s  %s\r\n", EventLog::format_time(e).c_str(), e.msg);
    }
  } else if (cmd == "print") {
    String err;
    if (!printer.enqueue(arg.toInt(), err)) Serial.printf("not queued: %s\r\n", err.c_str());
    else Serial.println("queued");
  } else if (cmd == "del") {
    Serial.println(jobs.remove(arg.toInt()) ? "deleted" : "no such job");
  } else if (cmd == "dump") {
    cmd_dump(arg.toInt());
  } else if (cmd == "text" || cmd == "render") {
    cmd_text(arg, cmd == "text");
  } else if (cmd == "putbegin") {
    LittleFS.remove("/spool/up.bin");
    File f = LittleFS.open("/spool/up.bin", "w");
    f.close();
    Serial.println("ok");
  } else if (cmd == "put") {
    File f = LittleFS.open("/spool/up.bin", "a");
    uint8_t b[1024];
    size_t n = 0;
    for (size_t i = 0; i + 1 < arg.length() && n < sizeof b; i += 2) b[n++] = strtoul(arg.substring(i, i + 2).c_str(), nullptr, 16);
    f.write(b, n);
    f.close();
    Serial.printf("ok %u\r\n", (unsigned)n);
  } else if (cmd == "putend") {
    String err;
    uint32_t id = jobs.add("/spool/up.bin", "serial", err);
    if (id) Serial.printf("job %lu\r\n", (unsigned long)id); else Serial.printf("add failed: %s\r\n", err.c_str());
  } else if (cmd == "httptest") {
    xTaskCreate(http_test_task, "httptest", 8192, nullptr, 1, nullptr);
  } else if (cmd == "btn") {
    int b = arg[0] == 'a' ? 0 : arg[0] == 'b' ? 1 : 2;
    display.press(b, arg.indexOf("hold") > 0);
    Serial.println("ok");
  } else if (cmd == "dinfo") {
    auto& d = M5.Display;
    d.fillRect(250, 30, 20, 20, TFT_RED);
    uint16_t a = d.readPixel(255, 35), b = d.readPixel(100, 35);
    Serial.printf("w %d h %d rot %d board %d  px(255,35)=%04x px(100,35)=%04x\r\n", d.width(), d.height(), d.getRotation(), (int)M5.getBoard(), a, b);
  } else if (cmd == "shot") {
    display.screenshot();
  } else if (cmd == "wifi") {
    int s2 = arg.indexOf(' ');
    settings.wifi_ssid = s2 < 0 ? arg : arg.substring(0, s2);
    settings.wifi_pass = s2 < 0 ? "" : arg.substring(s2 + 1);
    settings.save();
    Serial.printf("joining '%s'\r\n", settings.wifi_ssid.c_str());
    app_wifi_reconfigure();
  } else if (cmd == "host") {
    settings.hostname = arg;
    settings.save();
    app_wifi_reconfigure();
  } else if (cmd == "reboot") {
    app_request_reboot(200);
  } else {
    Serial.println("? (help)");
  }
}

void Console::task() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line_.length()) run(line_);
      line_ = "";
    } else if (line_.length() < 4096) {
      line_ += c;
    }
  }
}
