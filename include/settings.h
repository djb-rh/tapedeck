// Persistent configuration (NVS via Preferences).
#pragma once
#include <Arduino.h>

struct Settings {
  String wifi_ssid, wifi_pass;
  String hostname = "tapedeck";
  String web_pass;                    // empty = no password on settings/API writes
  String api_key;                     // empty = API open on the LAN; else X-API-Key header or ?key=
  String tz = "EST5EDT,M3.2.0,M11.1.0";
  String ntp = "pool.ntp.org";
  uint8_t margin_mm10 = 20;           // feed before/after each label, tenths of a mm (Brother min 2.0 mm)
  bool chain = false;                 // hold the last label in the printer (saves the next job's leader)
  uint8_t history_max = 25;
  uint16_t disp_timeout_s = 600;      // backlight off after this long idle; 0 = never
  uint8_t brightness = 140;
  uint8_t usb_cs = 0;                 // MAX3421E CS GPIO, 0 = auto-scan (13, 5, 0)
  uint8_t usb_int = 0;                // MAX3421E INT GPIO, 0 = auto-detect (35, 34)

  void load();
  void save() const;
  static void factory_reset();
};

extern Settings settings;
