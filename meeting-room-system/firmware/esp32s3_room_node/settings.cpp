#include "settings.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static Preferences prefs;
DeviceSettings g_settings;

static const char *NS = "roomnode";

static String trimSlash(String s) {
  s.trim();
  while (s.endsWith("/")) s.remove(s.length() - 1);
  return s;
}

void settingsBegin() {
  prefs.begin(NS, true);
  g_settings.ssid        = prefs.getString("ssid", "");
  g_settings.password    = prefs.getString("pass", "");
  g_settings.roomId      = prefs.getString("roomId", DEFAULT_ROOM_ID);
  g_settings.roomName    = prefs.getString("roomName", DEFAULT_ROOM_NAME);
  g_settings.serverUrl   = prefs.getString("server", DEFAULT_SERVER_URL);
  g_settings.deviceToken = prefs.getString("token", DEFAULT_DEVICE_TOKEN);
  prefs.end();
  g_settings.serverUrl = trimSlash(g_settings.serverUrl);
}

void settingsSave() {
  g_settings.serverUrl = trimSlash(g_settings.serverUrl);
  prefs.begin(NS, false);
  prefs.putString("ssid", g_settings.ssid);
  prefs.putString("pass", g_settings.password);
  prefs.putString("roomId", g_settings.roomId);
  prefs.putString("roomName", g_settings.roomName);
  prefs.putString("server", g_settings.serverUrl);
  prefs.putString("token", g_settings.deviceToken);
  prefs.end();
}

void settingsClearWiFi() {
  prefs.begin(NS, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  g_settings.ssid = "";
  g_settings.password = "";
}

bool settingsHasWiFi() { return g_settings.ssid.length() > 0; }

String deviceMac() { return WiFi.macAddress(); }

String deviceChipSuffix() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

String deviceApSsid() { return String(AP_SSID_PREFIX) + deviceChipSuffix(); }
