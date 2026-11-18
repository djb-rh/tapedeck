// TapeDeck: web label printing for a Brother P-touch (PT-D600) on an M5Stack
// Core Basic + USB Module (MAX3421E).
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_mac.h>
#include "app.h"
#include "console.h"
#include "display.h"
#include "jobs.h"
#include "printer.h"
#include "printer_usb.h"
#include "web_ui.h"

AppStatus app;
static uint32_t reboot_at = 0;
static bool mdns_up = false, ntp_started = false;
static uint32_t sta_started_ms = 0, last_sta_retry_ms = 0, ap_started_ms = 0;
static bool reconfigure = false;

void app_request_reboot(uint32_t delay_ms) { reboot_at = millis() + delay_ms; if (!reboot_at) reboot_at = 1; }

void app_factory_reset() {
  Settings::factory_reset();
  LittleFS.format();
  app_request_reboot(500);
}

String app_ip() {
  if (app.sta_connected) return WiFi.localIP().toString();
  if (app.ap_mode) return WiFi.softAPIP().toString();
  return "";
}

String app_uptime_str(uint32_t s) {
  char b[32];
  if (s >= 86400) snprintf(b, sizeof b, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
  else if (s >= 3600) snprintf(b, sizeof b, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
  else snprintf(b, sizeof b, "%lum %lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return b;
}

// ------------------------------------------------------------- Wi-Fi

static void start_ap() {
  if (app.ap_mode) return;
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char ssid[32];
  snprintf(ssid, sizeof ssid, "TapeDeck-%02X%02X", mac[4], mac[5]);
  app.ap_ssid = ssid;
  WiFi.mode(settings.wifi_ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(ssid);
  app.ap_mode = true;
  ap_started_ms = millis();
  webUi.set_captive(true);
  events.add("Setup Wi-Fi %s up at %s", ssid, WiFi.softAPIP().toString().c_str());
}

static void stop_ap() {
  if (!app.ap_mode) return;
  webUi.set_captive(false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  app.ap_mode = false;
  events.add("Setup Wi-Fi closed");
}

static void start_sta() {
  if (settings.wifi_ssid.isEmpty()) return;
  WiFi.setHostname(settings.hostname.c_str());
  // Several APs can serve one SSID; scan every channel and join the strongest
  // (the Arduino default joins the first one found).
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(settings.wifi_ssid.c_str(), settings.wifi_pass.c_str());
  sta_started_ms = last_sta_retry_ms = millis();
}

static void on_wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
  (void)info;
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      app.sta_connected = true;
      events.add("Wi-Fi connected to %s, IP %s, %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (app.sta_connected) events.add("Wi-Fi disconnected");
      app.sta_connected = false;
      mdns_up = false;
      break;
    default: break;
  }
}

void app_wifi_reconfigure() { reconfigure = true; }

static void wifi_task() {
  uint32_t now = millis();
  if (reconfigure) {
    reconfigure = false;
    delay(300);  // let the HTTP reply go out
    MDNS.end();
    mdns_up = false;
    WiFi.disconnect();
    app.sta_connected = false;
    if (!settings.wifi_ssid.isEmpty()) {
      if (app.ap_mode) WiFi.mode(WIFI_AP_STA);
      start_sta();
    }
    return;
  }
  if (app.sta_connected) {
    if (!mdns_up) {
      MDNS.end();
      if (MDNS.begin(settings.hostname.c_str())) {
        MDNS.setInstanceName("TapeDeck label printer");
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("tapedeck", "tcp", 80);
        MDNS.addServiceTxt("tapedeck", "tcp", "api", "/api/print");
        MDNS.addServiceTxt("tapedeck", "tcp", "version", TAPEDECK_VERSION);
        mdns_up = true;
      }
    }
    if (!ntp_started) {
      configTzTime(settings.tz.c_str(), settings.ntp.c_str());
      ntp_started = true;
    }
    // Keep the setup network up for two minutes after joining so the phone
    // that configured it sees the new address, then close it.
    if (app.ap_mode && now - ap_started_ms > 120000) stop_ap();
    return;
  }
  if (settings.wifi_ssid.isEmpty()) { start_ap(); return; }
  if (!app.ap_mode && now - sta_started_ms > 30000) start_ap();
  if (now - last_sta_retry_ms > 60000) {
    last_sta_retry_ms = now;
    WiFi.disconnect();
    delay(100);
    start_sta();
  }
}

// ------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\r\nTapeDeck " TAPEDECK_VERSION);
  app.boot_ms = millis();
  settings.load();

  // Find the USB Module before any SPI driver touches the bus.
  int cs = -1, intr = -1;
  PrinterUsb::detect_pins(cs, intr);
  if (settings.usb_cs) cs = settings.usb_cs;
  if (settings.usb_int) intr = settings.usb_int;
  if (cs >= 0 && intr < 0) intr = 35;  // the module's CH1 default

  auto cfg = M5.config();
  cfg.internal_spk = false;  // the Core Basic amp hums with USB traffic
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Speaker.end();
  pinMode(25, OUTPUT); digitalWrite(25, LOW);
  M5.Display.setBrightness(settings.brightness);

  events.begin();  // mounts LittleFS
  events.add("Boot %s, USB Module CS G%d INT G%d", TAPEDECK_VERSION, cs, intr);
  jobs.begin();
  display.begin();
  if (cs < 0) events.add("USB Module (MAX3421E) not found: check it is seated and its DIP switches");
  printer.begin(cs, intr);

  WiFi.onEvent(on_wifi_event);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  if (settings.wifi_ssid.isEmpty()) start_ap();
  else { WiFi.mode(WIFI_STA); start_sta(); }
  webUi.begin();
  console.begin();
}

void loop() {
  M5.update();
  webUi.task();
  wifi_task();
  display.task();
  console.task();
  if (reboot_at && (int32_t)(millis() - reboot_at) >= 0) {
    Serial.println("rebooting");
    delay(100);
    ESP.restart();
  }
  delay(2);
}
