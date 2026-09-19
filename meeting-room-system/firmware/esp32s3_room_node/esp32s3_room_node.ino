/*
 * =============================================================================
 *  회의실 예약/재실 관리 노드 - ESP32-S3-CAM (OV2640) + PIR
 * =============================================================================
 *
 *  동작 흐름
 *   1) 부팅 시 저장된 Wi-Fi 정보가 있으면 STA 모드로 접속을 시도한다.
 *   2) 정보가 없거나 접속에 실패하면 AP("RoomCam-XXXX") + 캡티브 포털을 연다.
 *      브라우저에서 SSID/비밀번호와 회의실 정보를 입력받는다.
 *   3) 입력한 AP에 접속되면 포털 화면에 "할당받은 IP"가 표시된다.
 *   4) 그 IP로 접속하면 회의실 실시간 영상 + 예약 화면이 나온다.
 *   5) 중앙 서버 주소를 넣어두면 재실 상태/스냅샷을 올리고 예약을 동기화한다.
 *
 *  필요한 것: Arduino IDE + esp32 보드 패키지 3.x (추가 라이브러리 없음)
 *  보드 설정: ESP32S3 Dev Module / PSRAM: OPI PSRAM / Partition: Huge APP
 * =============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "config.h"
#include "settings.h"
#include "camera.h"
#include "occupancy.h"
#include "reservations.h"
#include "timeutil.h"
#include "cloud.h"
#include "pages.h"

// ---------------------------------------------------------------------------
enum NetMode { MODE_PORTAL, MODE_STA };
enum WifiState { WS_IDLE = 0, WS_CONNECTING = 1, WS_CONNECTED = 2, WS_FAILED = 3 };

static WebServer server(80);
static DNSServer  dnsServer;
static const IPAddress AP_IP(192, 168, 4, 1);

static NetMode   g_mode         = MODE_PORTAL;
static WifiState g_wifiState    = WS_IDLE;
static uint32_t  g_connectStart = 0;
static uint32_t  g_rebootAt     = 0;   // 포털에서 접속 성공 후 재부팅 예정 시각
static uint32_t  g_portalStart  = 0;
static String    g_assignedIp   = "";
static String    g_mdnsHost     = "";

// ---------------------------------------------------------------------------
// 유틸
// ---------------------------------------------------------------------------
static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { /* 제어문자는 버림 */ }
        else out += c;
    }
  }
  return out;
}

static void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

static void sendErr(int code, const String &msg) {
  sendJson(code, String("{\"ok\":false,\"error\":\"") + jsonEscape(msg) + "\"}");
}

static String sanitizeHost(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c)) out += (char)tolower(c);
    else if (c == '-' || c == '_') out += '-';
  }
  if (!out.length()) out = "room";
  return out;
}

static void statusLed(bool on) {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, (STATUS_LED_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
#else
  (void)on;
#endif
}

// ---------------------------------------------------------------------------
// 공통 라우트 핸들러
// ---------------------------------------------------------------------------
static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  if (g_mode == MODE_PORTAL) server.send_P(200, "text/html; charset=utf-8", SETUP_PAGE);
  else                       server.send_P(200, "text/html; charset=utf-8", ROOM_PAGE);
}

static void handleInfo() {
  String j = "{";
  j += "\"ap\":\"" + jsonEscape(deviceApSsid()) + "\",";
  j += "\"fw\":\"" FIRMWARE_VERSION "\",";
  j += "\"camera\":" + String(cameraReady() ? "true" : "false") + ",";
  j += "\"ssid\":\"" + jsonEscape(g_settings.ssid) + "\",";
  j += "\"roomId\":\"" + jsonEscape(g_settings.roomId) + "\",";
  j += "\"roomName\":\"" + jsonEscape(g_settings.roomName) + "\",";
  j += "\"server\":\"" + jsonEscape(g_settings.serverUrl) + "\",";
  j += "\"token\":\"" + jsonEscape(g_settings.deviceToken) + "\"}";
  sendJson(200, j);
}

static void handleScan() {
  int n = WiFi.scanNetworks(false, true);
  String j = "{\"nets\":[";
  int emitted = 0;
  for (int i = 0; i < n && emitted < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int k = 0; k < i; k++) if (WiFi.SSID(k) == ssid) { dup = true; break; }
    if (dup) continue;
    if (emitted) j += ",";
    j += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"lock\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    emitted++;
  }
  j += "]}";
  WiFi.scanDelete();
  sendJson(200, j);
}

static void startConnecting(const String &ssid, const String &pass) {
  g_wifiState = WS_CONNECTING;
  g_connectStart = millis();
  g_assignedIp = "";
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

static void handleSave() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { sendErr(400, "SSID가 비어 있습니다."); return; }

  g_settings.ssid     = ssid;
  g_settings.password = server.arg("pass");
  if (server.hasArg("roomId") && server.arg("roomId").length())     g_settings.roomId = server.arg("roomId");
  if (server.hasArg("roomName") && server.arg("roomName").length()) g_settings.roomName = server.arg("roomName");
  if (server.hasArg("server"))                                      g_settings.serverUrl = server.arg("server");
  if (server.hasArg("token") && server.arg("token").length())       g_settings.deviceToken = server.arg("token");
  settingsSave();

  sendJson(200, "{\"ok\":true}");
  Serial.printf("[WIFI] '%s' 접속 시도\n", g_settings.ssid.c_str());
  startConnecting(g_settings.ssid, g_settings.password);
}

static void handleWifiStatus() {
  const char *st = "idle";
  if (g_wifiState == WS_CONNECTING) st = "connecting";
  else if (g_wifiState == WS_CONNECTED) st = "connected";
  else if (g_wifiState == WS_FAILED) st = "failed";

  String j = "{";
  j += "\"state\":\"" + String(st) + "\",";
  j += "\"ip\":\"" + jsonEscape(g_assignedIp) + "\",";
  j += "\"ssid\":\"" + jsonEscape(g_settings.ssid) + "\",";
  j += "\"host\":\"" + jsonEscape(g_mdnsHost) + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI());
  j += "}";
  sendJson(200, j);
}

// --- 상태 + 오늘 예약 -------------------------------------------------------
static String reservationJson(const Reservation *r) {
  String j = "{";
  j += "\"id\":\"" + jsonEscape(String(r->id)) + "\",";
  j += "\"title\":\"" + jsonEscape(String(r->title)) + "\",";
  j += "\"user\":\"" + jsonEscape(String(r->user)) + "\",";
  j += "\"date\":\"" + ymdToString(r->ymd) + "\",";
  j += "\"start\":" + String(r->startMin) + ",";
  j += "\"end\":" + String(r->endMin) + ",";
  j += "\"synced\":" + String((r->flags & RES_FLAG_SYNCED) ? "true" : "false");
  j += "}";
  return j;
}

static void handleStatus() {
  uint32_t ymd = timeTodayYmd();
  uint16_t nowMin = timeNowMinutes();

  String list = "[";
  int emitted = 0;
  // 시작 시각 순으로 정렬해서 출력 (개수가 적어 선택 정렬로 충분)
  int n = reservationCount();
  bool used[MAX_RESERVATIONS] = {false};
  for (int k = 0; k < n; k++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      const Reservation *r = reservationAt(i);
      if (used[i] || !r) continue;
      if (r->ymd != ymd || (r->flags & RES_FLAG_CANCELED)) continue;
      if (best < 0 || r->startMin < reservationAt(best)->startMin) best = i;
    }
    if (best < 0) break;
    used[best] = true;
    if (emitted) list += ",";
    list += reservationJson(reservationAt(best));
    emitted++;
  }
  list += "]";

  const Reservation *cur = reservationCurrent(ymd, nowMin);
  const Reservation *nxt = reservationNext(ymd, nowMin);
  uint32_t since = occupancySecondsSinceMotion();

  String j = "{";
  j += "\"roomId\":\"" + jsonEscape(g_settings.roomId) + "\",";
  j += "\"roomName\":\"" + jsonEscape(g_settings.roomName) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"time\":\"" + timeNowFull() + "\",";
  j += "\"date\":\"" + (ymd ? ymdToString(ymd) : String("")) + "\",";
  j += "\"nowMin\":" + String(nowMin) + ",";
  j += "\"timeSynced\":" + String(timeIsSynced() ? "true" : "false") + ",";
  j += "\"occupied\":" + String(occupancyIsOccupied() ? "true" : "false") + ",";
  j += "\"warmup\":" + String(occupancyWarmingUp() ? "true" : "false") + ",";
  j += "\"lastMotion\":" + String(since == 0xFFFFFFFFUL ? -1 : (long)since) + ",";
  j += "\"motionCount\":" + String(occupancyMotionCount()) + ",";
  j += "\"cameraReady\":" + String(cameraReady() ? "true" : "false") + ",";
  j += "\"streamPort\":" + String(CAMERA_HTTP_PORT) + ",";
  j += "\"uptime\":" + String(millis() / 1000UL) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"fw\":\"" FIRMWARE_VERSION "\",";
  j += "\"cloud\":{\"configured\":" + String(cloudConfigured() ? "true" : "false") +
       ",\"online\":" + String(cloudOnline() ? "true" : "false") +
       ",\"url\":\"" + jsonEscape(g_settings.serverUrl) + "\"" +
       ",\"lastSync\":" + String(cloudSecondsSinceSync() == 0xFFFFFFFFUL ? -1 : (long)cloudSecondsSinceSync()) +
       ",\"error\":\"" + jsonEscape(cloudLastError()) + "\"},";
  j += "\"current\":" + String(cur ? reservationJson(cur) : String("null")) + ",";
  j += "\"next\":" + String(nxt ? reservationJson(nxt) : String("null")) + ",";
  j += "\"reservations\":" + list;
  j += "}";
  sendJson(200, j);
}

static void handleReservationsGet() {
  uint32_t ymd = server.hasArg("date") ? stringToYmd(server.arg("date")) : timeTodayYmd();
  String j = "[";
  int emitted = 0;
  for (int i = 0; i < reservationCount(); i++) {
    const Reservation *r = reservationAt(i);
    if (!r || r->ymd != ymd || (r->flags & RES_FLAG_CANCELED)) continue;
    if (emitted) j += ",";
    j += reservationJson(r);
    emitted++;
  }
  j += "]";
  sendJson(200, j);
}

static void handleReservationCreate() {
  if (!timeIsSynced()) { sendErr(503, "시각 동기화 전에는 예약할 수 없습니다. 잠시 후 다시 시도하세요."); return; }

  uint32_t ymd = stringToYmd(server.arg("date"));
  if (!ymd) ymd = timeTodayYmd();
  int s = parseHm(server.arg("start"));
  int e = parseHm(server.arg("end"));
  if (s < 0 || e < 0) { sendErr(400, "시작/종료 시각을 입력하세요."); return; }
  if (ymd == timeTodayYmd() && e <= timeNowMinutes()) { sendErr(400, "이미 지난 시간입니다."); return; }

  String err;
  if (!reservationAdd(ymd, (uint16_t)s, (uint16_t)e, server.arg("title"), server.arg("user"), false, "", err)) {
    sendErr(409, err);
    return;
  }
  // 중앙 서버가 있으면 바로 밀어 올려서, 서버 쪽 예약과 겹칠 때 그 자리에서 알려준다.
  if (cloudConfigured() && cloudPushPending() == CLOUD_PUSH_REJECTED) {
    sendErr(409, cloudRejectReason());
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleQuickBook() {
  if (!timeIsSynced()) { sendErr(503, "시각 동기화 전에는 예약할 수 없습니다."); return; }
  uint32_t ymd = timeTodayYmd();
  uint16_t s = timeNowMinutes();
  uint16_t e = s + QUICK_BOOK_MINUTES;
  if (e > 1440) e = 1440;

  String err;
  String title = server.arg("title");
  if (!title.length()) title = "즉시 사용";
  if (!reservationAdd(ymd, s, e, title, server.arg("user"), false, "", err)) { sendErr(409, err); return; }
  if (cloudConfigured() && cloudPushPending() == CLOUD_PUSH_REJECTED) {
    sendErr(409, cloudRejectReason());
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleReservationCancel() {
  String id = server.arg("id");
  if (!id.length()) { sendErr(400, "예약 ID가 필요합니다."); return; }
  bool synced = false;
  for (int i = 0; i < reservationCount(); i++) {
    const Reservation *r = reservationAt(i);
    if (r && id.equals(r->id)) { synced = (r->flags & RES_FLAG_SYNCED); break; }
  }
  if (!reservationCancel(id)) { sendErr(404, "예약을 찾을 수 없습니다."); return; }
  if (synced && cloudConfigured()) cloudCancel(id);
  sendJson(200, "{\"ok\":true}");
}

static void handleWifiReset() {
  sendJson(200, "{\"ok\":true}");
  delay(300);
  settingsClearWiFi();
  ESP.restart();
}

static void handleRestart() {
  sendJson(200, "{\"ok\":true}");
  delay(300);
  ESP.restart();
}

static void handleNotFound() {
  if (g_mode == MODE_PORTAL) {
    // 캡티브 포털: 어떤 주소로 들어와도 설정 페이지로 보낸다.
    server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain; charset=utf-8", "not found");
}

static void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/save", HTTP_POST, handleSave);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reservations", HTTP_GET, handleReservationsGet);
  server.on("/api/reservations", HTTP_POST, handleReservationCreate);
  server.on("/api/reservations/cancel", HTTP_POST, handleReservationCancel);
  server.on("/api/quickbook", HTTP_POST, handleQuickBook);
  server.onNotFound(handleNotFound);
}

// ---------------------------------------------------------------------------
// 모드 전환
// ---------------------------------------------------------------------------
static void startPortal() {
  g_mode = MODE_PORTAL;
  g_portalStart = millis();
  WiFi.mode(WIFI_AP_STA);            // 설정 중에도 접속 시도를 할 수 있게 AP+STA
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  String ap = deviceApSsid();
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(ap.c_str(), AP_PASSWORD);
  else                          WiFi.softAP(ap.c_str());
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", AP_IP);

  Serial.println("========================================");
  Serial.printf(" 설정 모드: '%s' 에 접속 후\n", ap.c_str());
  Serial.printf(" 브라우저에서 http://%s/ 접속\n", AP_IP.toString().c_str());
  Serial.println("========================================");
}

static void startStaServices() {
  g_mode = MODE_STA;
  g_assignedIp = WiFi.localIP().toString();
  g_mdnsHost = sanitizeHost(g_settings.roomId);
  if (MDNS.begin(g_mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
  timeBegin();
  cameraStartServer();
  cloudBegin();

  Serial.println("========================================");
  Serial.printf(" 접속 완료! IP: %s\n", g_assignedIp.c_str());
  Serial.printf(" 회의실 페이지: http://%s/\n", g_assignedIp.c_str());
  Serial.printf(" 영상 스트림  : http://%s:%d/stream\n", g_assignedIp.c_str(), CAMERA_HTTP_PORT);
  Serial.printf(" mDNS         : http://%s.local/\n", g_mdnsHost.c_str());
  Serial.println("========================================");
}

static bool tryConnectSaved() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(g_settings.ssid.c_str(), g_settings.password.c_str());
  Serial.printf("[WIFI] 저장된 AP '%s' 접속 시도", g_settings.ssid.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) { Serial.println(" → 성공"); return true; }
    statusLed(((millis() / 250) % 2) == 0);
    delay(100);
    Serial.print(".");
  }
  Serial.println(" → 실패");
  return false;
}

// ---------------------------------------------------------------------------
// BOOT 버튼 길게 눌러 초기화
// ---------------------------------------------------------------------------
static void resetButtonLoop() {
#if RESET_BUTTON_PIN >= 0
  static uint32_t pressedAt = 0;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    else if (millis() - pressedAt > 3000) {
      Serial.println("[SYS] Wi-Fi 설정 초기화 후 재부팅");
      settingsClearWiFi();
      delay(200);
      ESP.restart();
    }
  } else {
    pressedAt = 0;
  }
#endif
}

// ---------------------------------------------------------------------------
static void ledLoop() {
#if STATUS_LED_PIN >= 0
  uint32_t now = millis();
  if (g_mode == MODE_PORTAL)          statusLed(((now / 200) % 2) == 0);     // 빠른 점멸 = 설정 모드
  else if (WiFi.status() != WL_CONNECTED) statusLed(((now / 600) % 2) == 0); // 느린 점멸 = 재접속 중
  else if (occupancyIsOccupied())     statusLed(true);                       // 상시 점등 = 사용중
  else                                statusLed((now % 3000) < 60);          // 짧은 깜빡임 = 대기
#endif
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[SYS] 회의실 노드 부팅 " FIRMWARE_VERSION);

#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
#endif
#if RESET_BUTTON_PIN >= 0
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
#endif
#if FLASH_LED_PIN >= 0
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
#endif

  settingsBegin();
  reservationsBegin();
  occupancyBegin();
  cameraBegin();                   // 실패해도 예약 기능은 계속 동작한다

  WiFi.persistent(false);
  WiFi.setHostname(sanitizeHost(g_settings.roomId).c_str());

  bool connected = false;
  if (settingsHasWiFi()) connected = tryConnectSaved();

  setupRoutes();
  server.begin();

  if (connected) {
    g_wifiState = WS_CONNECTED;
    startStaServices();
  } else {
    startPortal();
    cameraStartServer();           // 설정 중에도 카메라 각도를 맞출 수 있게
  }
}

void loop() {
  server.handleClient();
  if (g_mode == MODE_PORTAL) dnsServer.processNextRequest();

  occupancyLoop();
  resetButtonLoop();
  ledLoop();

  // 포털에서 입력받은 AP로 접속 중인 경우의 상태 추적
  if (g_mode == MODE_PORTAL && g_wifiState == WS_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiState  = WS_CONNECTED;
      g_assignedIp = WiFi.localIP().toString();
      g_mdnsHost   = sanitizeHost(g_settings.roomId);
      g_rebootAt   = millis() + 20000UL;   // IP를 보여줄 시간을 준 뒤 STA 모드로 재부팅
      Serial.printf("[WIFI] 접속 성공, 할당 IP = %s\n", g_assignedIp.c_str());
    } else if (millis() - g_connectStart > STA_CONNECT_TIMEOUT_MS) {
      g_wifiState = WS_FAILED;
      WiFi.disconnect(false, false);
      Serial.println("[WIFI] 접속 실패");
    }
  }

  if (g_rebootAt && millis() > g_rebootAt) {
    Serial.println("[SYS] STA 모드로 재부팅합니다.");
    delay(100);
    ESP.restart();
  }

  // 설정 포털을 열어둔 채 방치되면 재부팅해서 다시 접속을 시도한다.
  if (g_mode == MODE_PORTAL && g_wifiState == WS_IDLE && settingsHasWiFi() &&
      millis() - g_portalStart > PORTAL_IDLE_REBOOT_MS) {
    ESP.restart();
  }

  if (g_mode == MODE_STA) {
    // Wi-Fi가 끊기면 자동 재접속
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 5000) {
      lastCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] 연결이 끊겨 재접속 시도");
        WiFi.disconnect();
        WiFi.begin(g_settings.ssid.c_str(), g_settings.password.c_str());
      }
    }
    cloudLoop();

    // 자정이 지나면 지난 예약 정리
    static uint32_t lastYmd = 0;
    uint32_t ymd = timeTodayYmd();
    if (ymd && ymd != lastYmd) {
      lastYmd = ymd;
      reservationsPurgeBefore(ymd);
      cloudRequestPull();
    }
  }
}
