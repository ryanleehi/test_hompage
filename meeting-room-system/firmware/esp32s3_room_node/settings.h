/*
 * settings.h - NVS(Preferences)에 저장되는 사용자 설정
 */
#pragma once
#include <Arduino.h>

struct DeviceSettings {
  String ssid;
  String password;
  String roomId;
  String roomName;
  String serverUrl;    // 예: http://192.168.0.10:3000 (끝의 / 는 제거해서 보관)
  String deviceToken;
};

extern DeviceSettings g_settings;

void settingsBegin();          // NVS에서 읽어 g_settings 채움 (없으면 config.h 기본값)
void settingsSave();           // g_settings 를 NVS에 기록
void settingsClearWiFi();      // SSID/비밀번호만 삭제 (회의실 정보는 유지)
bool settingsHasWiFi();
String deviceChipSuffix();     // MAC 하위 2바이트 → "A1B2"
String deviceApSsid();         // "RoomCam-A1B2"
String deviceMac();
