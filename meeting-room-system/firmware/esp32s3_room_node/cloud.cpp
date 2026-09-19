#include "cloud.h"
#include "config.h"
#include "settings.h"
#include "reservations.h"
#include "timeutil.h"
#include "occupancy.h"
#include "camera.h"

#include <WiFi.h>
#include <HTTPClient.h>

static uint32_t s_lastHeartbeat = 0;
static uint32_t s_lastPull      = 0;
static uint32_t s_lastSnapshot  = 0;
static uint32_t s_lastOkMs      = 0;
static bool     s_online        = false;
static bool     s_registered    = false;
static bool     s_pullNow       = true;
static String   s_lastError     = "";
static String   s_rejectReason  = "";

bool   cloudConfigured() { return g_settings.serverUrl.length() > 0; }
bool   cloudOnline()     { return s_online; }
String cloudLastError()  { return s_lastError; }
String cloudRejectReason() { return s_rejectReason; }

uint32_t cloudSecondsSinceSync() {
  if (s_lastOkMs == 0) return 0xFFFFFFFFUL;
  return (millis() - s_lastOkMs) / 1000UL;
}

void cloudRequestPull() { s_pullNow = true; }

static String urlEncode(const String &src) {
  String out;
  out.reserve(src.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < src.length(); i++) {
    char c = src[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static void markResult(bool ok, const String &what, int code) {
  if (ok) {
    s_online   = true;
    s_lastOkMs = millis();
    s_lastError = "";
  } else {
    s_online = false;
    s_lastError = what + " 실패(" + String(code) + ")";
  }
}

// 공통 POST (application/x-www-form-urlencoded). 응답 본문을 반환한다.
static bool httpPostForm(const String &path, const String &body, String &responseOut, int &codeOut) {
  if (!cloudConfigured() || WiFi.status() != WL_CONNECTED) { codeOut = -1; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, g_settings.serverUrl + path)) { codeOut = -2; return false; }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("X-Device-Token", g_settings.deviceToken);
  codeOut = http.POST(body);
  bool ok = (codeOut >= 200 && codeOut < 300);
  responseOut = ok ? http.getString() : "";
  http.end();
  return ok;
}

static bool httpGetText(const String &path, String &responseOut, int &codeOut) {
  if (!cloudConfigured() || WiFi.status() != WL_CONNECTED) { codeOut = -1; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, g_settings.serverUrl + path)) { codeOut = -2; return false; }
  http.addHeader("X-Device-Token", g_settings.deviceToken);
  codeOut = http.GET();
  bool ok = (codeOut >= 200 && codeOut < 300);
  responseOut = ok ? http.getString() : "";
  http.end();
  return ok;
}

static String baseBody() {
  return "roomId=" + urlEncode(g_settings.roomId);
}

// --- 기기 등록 -------------------------------------------------------------
static void doRegister() {
  String body = baseBody();
  body += "&roomName=" + urlEncode(g_settings.roomName);
  body += "&ip=" + urlEncode(WiFi.localIP().toString());
  body += "&mac=" + urlEncode(deviceMac());
  body += "&fw=" + urlEncode(FIRMWARE_VERSION);
  body += "&streamPort=" + String(CAMERA_HTTP_PORT);
  body += "&hasCamera=" + String(cameraReady() ? 1 : 0);

  String resp; int code;
  bool ok = httpPostForm("/api/devices/register", body, resp, code);
  markResult(ok, "기기 등록", code);
  if (ok) {
    s_registered = true;
    s_pullNow = true;
    Serial.println("[CLOUD] 서버에 기기 등록 완료");
  }
}

// --- 하트비트 --------------------------------------------------------------
static void doHeartbeat() {
  uint32_t since = occupancySecondsSinceMotion();
  String body = baseBody();
  body += "&roomName=" + urlEncode(g_settings.roomName);
  body += "&occupied=" + String(occupancyIsOccupied() ? 1 : 0);
  body += "&lastMotion=" + String(since == 0xFFFFFFFFUL ? -1 : (long)since);
  body += "&motionCount=" + String(occupancyMotionCount());
  body += "&rssi=" + String(WiFi.RSSI());
  body += "&ip=" + urlEncode(WiFi.localIP().toString());
  body += "&uptime=" + String(millis() / 1000UL);
  body += "&heap=" + String(ESP.getFreeHeap());
  body += "&fw=" + urlEncode(FIRMWARE_VERSION);
  body += "&streamPort=" + String(CAMERA_HTTP_PORT);

  String resp; int code;
  bool ok = httpPostForm("/api/devices/heartbeat", body, resp, code);
  markResult(ok, "하트비트", code);
  if (ok && resp.indexOf("PULL|1") >= 0) s_pullNow = true;   // 서버 쪽 예약이 바뀜
}

// --- 예약 목록 내려받기 ----------------------------------------------------

// "RES|id|start|end|title|user" 한 줄을 6개 필드로 나눈다.
static bool parseResLine(const String &line, String out[6]) {
  if (!line.startsWith("RES|")) return false;
  int idx = 0, from = 0;
  while (idx < 6) {
    int bar = line.indexOf('|', from);
    if (bar < 0) { out[idx++] = line.substring(from); break; }
    out[idx++] = line.substring(from, bar);
    from = bar + 1;
  }
  return idx == 6;
}

static void doPull() {
  uint32_t ymd = timeTodayYmd();
  if (ymd == 0) return;                 // 시각 동기화 전에는 의미가 없다

  String path = "/api/devices/reservations?roomId=" + urlEncode(g_settings.roomId) +
                "&date=" + timeTodayString();
  String resp; int code;
  if (!httpGetText(path, resp, code)) {
    markResult(false, "예약 조회", code);
    return;
  }
  markResult(true, "예약 조회", code);

  // 1) 서버 목록을 기기 저장분과 같은 형식의 지문으로 만든다.
  String fpServer;
  fpServer.reserve(256);
  int pos = 0;
  while (pos < (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    String line = (nl < 0) ? resp.substring(pos) : resp.substring(pos, nl);
    pos = (nl < 0) ? resp.length() : nl + 1;
    line.trim();
    String f[6];
    if (!parseResLine(line, f)) continue;
    fpServer += f[1]; fpServer += '|';
    fpServer += f[2]; fpServer += '|';
    fpServer += f[3]; fpServer += '|';
    fpServer += f[4]; fpServer += '|';
    fpServer += f[5]; fpServer += '\n';
  }

  // 2) 바뀐 게 없으면 NVS를 건드리지 않는다. (60초마다 쓰면 수명이 아깝다)
  if (fpServer == reservationsFingerprint(ymd)) return;

  // 3) 달라졌을 때만 통째로 교체하고 한 번만 저장한다.
  reservationsSetAutoSave(false);
  reservationsClearSyncedFor(ymd);

  pos = 0;
  while (pos < (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    String line = (nl < 0) ? resp.substring(pos) : resp.substring(pos, nl);
    pos = (nl < 0) ? resp.length() : nl + 1;
    line.trim();
    String f[6];
    if (!parseResLine(line, f)) continue;
    String err;
    reservationAdd(ymd, (uint16_t)f[2].toInt(), (uint16_t)f[3].toInt(), f[4], f[5], true, f[1], err);
  }
  reservationsPurgeBefore(ymd);
  reservationsSetAutoSave(true);
  reservationsSave();
  Serial.println("[CLOUD] 예약 목록이 갱신되었습니다.");
}

// --- 로컬에서 만든 예약을 서버로 올리기 ------------------------------------
CloudPushResult cloudPushPending() {
  int idx = reservationFirstPendingIndex();
  if (idx < 0) return CLOUD_PUSH_NONE;
  const Reservation *r = reservationAt(idx);
  if (!r) return CLOUD_PUSH_NONE;

  String body = baseBody();
  body += "&date=" + ymdToString(r->ymd);
  body += "&start=" + String(r->startMin);
  body += "&end=" + String(r->endMin);
  body += "&title=" + urlEncode(String(r->title));
  body += "&user=" + urlEncode(String(r->user));
  body += "&localId=" + urlEncode(String(r->id));

  String resp; int code;
  if (!httpPostForm("/api/devices/reservations", body, resp, code)) {
    // 409(시간 겹침 등)이면 서버가 원본이므로 로컬 예약을 버린다.
    if (code == 409) {
      s_rejectReason = "중앙 서버에 이미 겹치는 예약이 있습니다.";
      Serial.println("[CLOUD] 서버가 예약을 거절해 로컬 예약을 삭제합니다.");
      reservationRemove(String(r->id));
      s_pullNow = true;
      return CLOUD_PUSH_REJECTED;
    }
    markResult(false, "예약 업로드", code);
    return CLOUD_PUSH_OFFLINE;
  }
  markResult(true, "예약 업로드", code);
  s_rejectReason = "";

  String serverId;
  int bar = resp.indexOf('|');
  if (resp.startsWith("OK") && bar >= 0) {
    serverId = resp.substring(bar + 1);
    serverId.trim();
  }
  reservationMarkSynced(idx, serverId);
  return CLOUD_PUSH_OK;
}

bool cloudCancel(const String &id) {
  if (!cloudConfigured()) return false;
  String body = baseBody() + "&id=" + urlEncode(id);
  String resp; int code;
  bool ok = httpPostForm("/api/devices/cancel", body, resp, code);
  markResult(ok, "예약 취소", code);
  if (ok) s_pullNow = true;
  return ok;
}

// --- 대시보드용 스냅샷 업로드 ----------------------------------------------
static void doSnapshot() {
  if (!cameraReady()) return;
  camera_fb_t *fb = cameraGrab();
  if (!fb) return;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  String url = g_settings.serverUrl + "/api/devices/snapshot?roomId=" + urlEncode(g_settings.roomId);
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device-Token", g_settings.deviceToken);
    int code = http.POST(fb->buf, fb->len);
    http.end();
    if (code < 200 || code >= 300) {
      Serial.printf("[CLOUD] 스냅샷 업로드 실패 (%d)\n", code);
    }
  }
  cameraRelease(fb);
}

void cloudBegin() {
  s_lastHeartbeat = s_lastPull = s_lastSnapshot = 0;
  s_registered = false;
  s_pullNow = true;
}

void cloudLoop() {
  if (!cloudConfigured() || WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();

  if (!s_registered) {
    static uint32_t lastTry = 0;
    if (now - lastTry > 10000UL || lastTry == 0) {
      lastTry = now;
      doRegister();
    }
    if (!s_registered) return;
  }

  if (now - s_lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    s_lastHeartbeat = now;
    doHeartbeat();
    cloudPushPending();
  }

  if (s_pullNow || (now - s_lastPull >= PULL_INTERVAL_MS)) {
    s_lastPull = now;
    s_pullNow = false;
    doPull();
  }

#if SNAPSHOT_INTERVAL_MS > 0
  if (now - s_lastSnapshot >= SNAPSHOT_INTERVAL_MS) {
    s_lastSnapshot = now;
    doSnapshot();
  }
#endif
}
