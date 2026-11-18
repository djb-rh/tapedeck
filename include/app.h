// Shared application state between main, web UI and display.
#pragma once
#include <Arduino.h>
#include "settings.h"
#include "events.h"

struct AppStatus {
  bool ap_mode = false;        // captive portal AP is up
  bool sta_connected = false;
  String ap_ssid;
  uint32_t boot_ms = 0;
  uint32_t last_web_ms = 0;    // last API/editor request (display shows activity)
};

extern AppStatus app;

void app_request_reboot(uint32_t delay_ms = 500);
void app_factory_reset();
void app_wifi_reconfigure();
String app_ip();
String app_uptime_str(uint32_t secs);
