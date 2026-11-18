#include "settings.h"
#include <Preferences.h>

Settings settings;
static const char* NS = "tapedeck";

void Settings::load() {
  Preferences p;
  if (!p.begin(NS, true)) return;
  wifi_ssid = p.getString("ssid", wifi_ssid);
  wifi_pass = p.getString("pass", wifi_pass);
  hostname = p.getString("host", hostname);
  web_pass = p.getString("webpass", web_pass);
  api_key = p.getString("apikey", api_key);
  tz = p.getString("tz", tz);
  ntp = p.getString("ntp", ntp);
  margin_mm10 = p.getUChar("margin", margin_mm10);
  chain = p.getBool("chain", chain);
  history_max = p.getUChar("histmax", history_max);
  disp_timeout_s = p.getUShort("dispto", disp_timeout_s);
  brightness = p.getUChar("bright", brightness);
  usb_cs = p.getUChar("usbcs", usb_cs);
  usb_int = p.getUChar("usbint", usb_int);
  p.end();
  if (hostname.isEmpty()) hostname = "tapedeck";
  if (margin_mm10 < 20) margin_mm10 = 20;
  if (history_max < 1) history_max = 1;
  if (history_max > 50) history_max = 50;
}

void Settings::save() const {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putString("ssid", wifi_ssid);
  p.putString("pass", wifi_pass);
  p.putString("host", hostname);
  p.putString("webpass", web_pass);
  p.putString("apikey", api_key);
  p.putString("tz", tz);
  p.putString("ntp", ntp);
  p.putUChar("margin", margin_mm10);
  p.putBool("chain", chain);
  p.putUChar("histmax", history_max);
  p.putUShort("dispto", disp_timeout_s);
  p.putUChar("bright", brightness);
  p.putUChar("usbcs", usb_cs);
  p.putUChar("usbint", usb_int);
  p.end();
}

void Settings::factory_reset() {
  Preferences p;
  if (p.begin(NS, false)) { p.clear(); p.end(); }
}
