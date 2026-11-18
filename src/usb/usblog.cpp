// RAM ring buffer for TinyUSB's debug log (app-debug env), readable over
// Wi-Fi at /api/usblog, so enumeration problems can be caught without a
// serial cable. In normal builds nothing calls tapedeck_usb_log.
#include <Arduino.h>
#include <stdarg.h>

static constexpr size_t kCap = 32 * 1024;
static char* g_buf = nullptr;
static size_t g_head = 0;   // next write position
static bool g_wrapped = false;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" int tapedeck_usb_log(const char* fmt, ...) {
  char tmp[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n <= 0) return n;
  if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
  if (!g_buf) {
    char* b = (char*)malloc(kCap);
    if (!b) return n;
    portENTER_CRITICAL(&g_mux);
    if (!g_buf) { g_buf = b; b = nullptr; }
    portEXIT_CRITICAL(&g_mux);
    if (b) free(b);
  }
  char stamp[16];
  static bool at_line_start = true;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < n; ++i) {
    if (at_line_start) {
      int sl = snprintf(stamp, sizeof stamp, "%7lu ", (unsigned long)millis());
      for (int k = 0; k < sl; ++k) { g_buf[g_head++] = stamp[k]; if (g_head == kCap) { g_head = 0; g_wrapped = true; } }
      at_line_start = false;
    }
    char ch = tmp[i];
    if (ch == '\r') continue;
    g_buf[g_head++] = ch;
    if (g_head == kCap) { g_head = 0; g_wrapped = true; }
    if (ch == '\n') at_line_start = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return n;
}

// Copies the log, oldest first, into a String.
String tapedeck_usb_log_text() {
  String out;
  if (!g_buf) return out;
  size_t len = g_wrapped ? kCap : g_head;
  char* copy = (char*)malloc(len + 1);
  if (!copy) return "out of memory";
  portENTER_CRITICAL(&g_mux);
  if (g_wrapped) {
    memcpy(copy, g_buf + g_head, kCap - g_head);
    memcpy(copy + (kCap - g_head), g_buf, g_head);
  } else {
    memcpy(copy, g_buf, g_head);
  }
  portEXIT_CRITICAL(&g_mux);
  copy[len] = 0;
  out = copy;
  free(copy);
  return out;
}

void tapedeck_usb_log_clear() {
  portENTER_CRITICAL(&g_mux);
  g_head = 0;
  g_wrapped = false;
  portEXIT_CRITICAL(&g_mux);
}
