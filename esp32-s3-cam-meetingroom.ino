/*
 * ESP32-S3-CAM 회의실 재실 감지 클라이언트  (v2.1 - WiFiManager 라이브러리 기반)
 * ==========================================================================
 *
 * 기능
 *  - 부팅 시 AP 모드( SSID: MeetingCam-XXXX / 비밀번호 12345678 ) 를 띄움.
 *    192.168.4.1 접속 -> [WiFi 설정] -> 주변 SSID 목록에서 선택 + 비밀번호 입력,
 *    그 아래에 서버 IP / 포트 / 회의실명 입력  (WiFiManager 라이브러리의 설정 포털)
 *  - 저장하면 접속을 시도하고, 같은 화면에 접속된 IP 가 표시됨 (AP 는 계속 유지)
 *  - 보드 IP 에 접속하면 얼굴/사람이 모자이크(블러) 처리된 카메라 영상이 보임
 *      http://<ip>/          : 메인 페이지 (영상 + 상태 + 서버 인증코드 입력)
 *      http://<ip>/wifi      : WiFi / 서버 / 회의실명 설정 (WiFiManager)
 *      http://<ip>:81/stream : MJPEG 스트림
 *      http://<ip>/capture   : 정지 사진(JPEG)
 *  - 얼굴이 감지되면 "재실" 로 판단하고, 서버(server.py)에 주기적으로 상태(heartbeat)와
 *    블러 처리된 스냅샷을 전송함
 *  - 서버는 MAC/IP 를 clients.csv 에 기록하고 인증코드를 발급함. 메인 페이지에서
 *    인증코드를 입력하고 [인증 시도] 를 누르면 서버가 확인 후 토큰을 발급함
 *
 * 필요한 라이브러리 (Arduino IDE > 라이브러리 매니저)
 *  - "WiFiManager" by tzapu  (2.0.17 이상)
 *  나머지(WiFi, WebServer, HTTPClient, Preferences, ESPmDNS, esp_camera, esp-dl)는 ESP32 코어 포함
 *
 * Arduino IDE 설정 (ESP32 보드 패키지 3.x 기준)
 *  - 보드: "ESP32S3 Dev Module" (또는 Freenove/XIAO 등 보드에 맞는 항목)
 *  - PSRAM: 모듈 라벨이 N8R8/N16R8 -> "OPI PSRAM",  N4R2/N8R2 -> "QSPI PSRAM"  (얼굴 감지에 필수)
 *  - Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
 *  - Flash Size: 보드에 맞게 (보통 8MB / 16MB)
 *  - USB CDC On Boot: Enabled (시리얼 로그 확인용)
 */

#include "esp_camera.h"
#include "img_converters.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <HTTPClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "esp_mac.h"             // esp_read_mac(): WiFi 초기화와 무관하게 eFuse 의 고유 MAC 을 읽음
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// =====================================================================
// 보드(카메라 핀) 선택 - 사용하는 보드 하나만 주석 해제
// =====================================================================
#define CAMERA_MODEL_ESP32S3_EYE      // Freenove ESP32-S3-WROOM CAM, 대부분의 "ESP32-S3-CAM" 보드
// #define CAMERA_MODEL_XIAO_ESP32S3  // Seeed XIAO ESP32S3 Sense
// #define CAMERA_MODEL_CUSTOM        // 아래 CUSTOM 핀을 직접 수정

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  15
  #define SIOD_GPIO_NUM  4
  #define SIOC_GPIO_NUM  5
  #define Y9_GPIO_NUM    16
  #define Y8_GPIO_NUM    17
  #define Y7_GPIO_NUM    18
  #define Y6_GPIO_NUM    12
  #define Y5_GPIO_NUM    10
  #define Y4_GPIO_NUM    8
  #define Y3_GPIO_NUM    9
  #define Y2_GPIO_NUM    11
  #define VSYNC_GPIO_NUM 6
  #define HREF_GPIO_NUM  7
  #define PCLK_GPIO_NUM  13
#elif defined(CAMERA_MODEL_XIAO_ESP32S3)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  10
  #define SIOD_GPIO_NUM  40
  #define SIOC_GPIO_NUM  39
  #define Y9_GPIO_NUM    48
  #define Y8_GPIO_NUM    11
  #define Y7_GPIO_NUM    12
  #define Y6_GPIO_NUM    14
  #define Y5_GPIO_NUM    16
  #define Y4_GPIO_NUM    18
  #define Y3_GPIO_NUM    17
  #define Y2_GPIO_NUM    15
  #define VSYNC_GPIO_NUM 38
  #define HREF_GPIO_NUM  47
  #define PCLK_GPIO_NUM  13
#elif defined(CAMERA_MODEL_CUSTOM)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  15
  #define SIOD_GPIO_NUM  4
  #define SIOC_GPIO_NUM  5
  #define Y9_GPIO_NUM    16
  #define Y8_GPIO_NUM    17
  #define Y7_GPIO_NUM    18
  #define Y6_GPIO_NUM    12
  #define Y5_GPIO_NUM    10
  #define Y4_GPIO_NUM    8
  #define Y3_GPIO_NUM    9
  #define Y2_GPIO_NUM    11
  #define VSYNC_GPIO_NUM 6
  #define HREF_GPIO_NUM  7
  #define PCLK_GPIO_NUM  13
#else
  #error "카메라 모델을 선택하세요"
#endif

// =====================================================================
// 동작 설정
// =====================================================================
#define FW_VERSION            "2.1"
#define AP_SSID_PREFIX        "MeetingCam-"   // AP 이름: MeetingCam-<MAC 뒤 4자리>
#define AP_PASSWORD           "12345678"      // 8자 이상. 개방형 AP 를 원하면 "" 로
#define KEEP_AP_AFTER_CONNECT true            // STA 접속 후에도 AP/설정 포털 유지 (재설정 편의)
#define DEFAULT_SERVER_PORT   5000
#define HEARTBEAT_INTERVAL_MS 10000UL         // 서버 상태 보고 주기
#define SNAPSHOT_INTERVAL_MS  3000UL          // 서버 스냅샷 전송 주기
#define REGISTER_INTERVAL_MS  30000UL         // 미인증 상태에서 서버 등록 재시도 주기
#define PRESENCE_HOLD_MS      30000UL         // 마지막 얼굴 감지 후 "재실" 유지 시간
#define STA_CONNECT_TIMEOUT_S 20              // WiFiManager 접속 시도 제한 시간(초)
#define STREAM_JPEG_QUALITY   80              // fmt2jpg 품질 (0~100)
#define CAM_VFLIP             0
#define CAM_HMIRROR           0
#define BLUR_PERSON           1               // 1: 얼굴을 기준으로 사람 전체(머리~몸통)를 모자이크, 0: 얼굴만

// 얼굴 감지 가능 여부 (ESP32-S3 + PSRAM + 코어에 esp-dl 포함)
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BOARD_HAS_PSRAM) && __has_include("human_face_detect_msr01.hpp")
  #define FACE_DETECT_AVAILABLE 1
  #include <list>
  #include <vector>
  #include "human_face_detect_msr01.hpp"
  #include "human_face_detect_mnp01.hpp"
#else
  #define FACE_DETECT_AVAILABLE 0
  #warning "얼굴 감지를 사용할 수 없습니다. 보드 설정에서 PSRAM(OPI/QSPI PSRAM)을 켜고 ESP32-S3 보드를 선택하세요."
#endif

// =====================================================================
// 전역 상태
// =====================================================================
Preferences    prefs;
WiFiManager    wm;
httpd_handle_t streamHttpd = nullptr;

// WiFiManager 가 만드는 웹서버(포트 80)에 우리 페이지를 얹는다
#define web (*wm.server)

// 설정 포털의 SSID/비밀번호 입력란 아래에 붙는 추가 입력란
WiFiManagerParameter *pServer = nullptr;
WiFiManagerParameter *pPort   = nullptr;
WiFiManagerParameter *pRoom   = nullptr;

struct Config {
  String serverHost;
  int    serverPort = DEFAULT_SERVER_PORT;
  String room;
  String token;
} cfg;                   // WiFi SSID/비밀번호는 WiFiManager 가 NVS 에 직접 저장

String  g_mac;           // STA MAC (기기 식별자)
String  g_apSsid;
String  g_hostname;

bool          g_wasConnected   = false;
unsigned long g_lastPortalTry  = 0;

bool          g_authorized     = false;
bool          g_serverOk       = false;
String        g_serverMsg      = "서버 미접속";
unsigned long g_lastHeartbeat  = 0;
unsigned long g_lastSnapshot   = 0;
unsigned long g_lastRegister   = 0;

// 카메라 / 얼굴 감지 공유 상태
struct SharedFrame {
  uint8_t *buf = nullptr;
  size_t   len = 0;
  uint32_t seq = 0;
};
SharedFrame       g_frame;
SemaphoreHandle_t g_frameMutex;
volatile int           g_faces       = 0;
volatile unsigned long g_lastFaceMs  = 0;
volatile bool          g_camReady    = false;
volatile float         g_fps         = 0;

// =====================================================================
// 유틸리티
// =====================================================================
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

// 아주 단순한 flat JSON 값 추출 ( {"key":"value"} / {"key":123} / {"key":true} )
static String jsonGet(const String &body, const char *key) {
  String k = String("\"") + key + "\"";
  int i = body.indexOf(k);
  if (i < 0) return "";
  i = body.indexOf(':', i + k.length());
  if (i < 0) return "";
  i++;
  while (i < (int)body.length() && body[i] == ' ') i++;
  if (i >= (int)body.length()) return "";
  if (body[i] == '"') {
    String out;
    int j = i + 1;
    while (j < (int)body.length() && body[j] != '"') {
      if (body[j] == '\\' && j + 1 < (int)body.length()) j++;
      out += body[j];
      j++;
    }
    return out;
  }
  int j = i;
  while (j < (int)body.length() && body[j] != ',' && body[j] != '}' && body[j] != ']') j++;
  String v = body.substring(i, j);
  v.trim();
  return v;
}

static bool occupiedNow() {
  return g_lastFaceMs != 0 && (millis() - g_lastFaceMs) < PRESENCE_HOLD_MS;
}

static bool detectAvailable() {
  return FACE_DETECT_AVAILABLE && psramFound();
}

static String staIp() {
  return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
}

static String serverBaseUrl() {
  return "http://" + cfg.serverHost + ":" + String(cfg.serverPort);
}

static bool serverConfigured() {
  return cfg.serverHost.length() > 0;
}

static const char *apPassword() {
  return strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : nullptr;   // 8자 미만이면 개방형 AP
}

// =====================================================================
// 설정 저장/로드 (서버/회의실/토큰 - WiFi 자격증명은 WiFiManager 담당)
// =====================================================================
static void loadConfig() {
  prefs.begin("meetcam", true);
  cfg.serverHost = prefs.getString("server", "");
  cfg.serverPort = prefs.getInt("port", DEFAULT_SERVER_PORT);
  cfg.room       = prefs.getString("room", "");
  cfg.token      = prefs.getString("token", "");
  prefs.end();
}

static void saveConfig() {
  prefs.begin("meetcam", false);
  prefs.putString("server", cfg.serverHost);
  prefs.putInt("port", cfg.serverPort);
  prefs.putString("room", cfg.room);
  prefs.putString("token", cfg.token);
  prefs.end();
}

static void saveToken(const String &t) {
  cfg.token = t;
  prefs.begin("meetcam", false);
  prefs.putString("token", cfg.token);
  prefs.end();
}

// =====================================================================
// 카메라 + 얼굴/사람 모자이크
// =====================================================================
static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.jpeg_quality = 12;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  bool psram = psramFound();
  if (FACE_DETECT_AVAILABLE && psram) {
    // 얼굴 감지: RGB565 입력이 필요하고, 폭이 400px 이하일 때 실용적인 속도가 나옴
    // (QVGA RGB565 한 장 = 150KB 이므로 PSRAM 필수)
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size   = FRAMESIZE_QVGA;     // 320x240
    c.fb_count     = 2;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
  } else if (psram) {
    // PSRAM 은 있지만 얼굴 감지 라이브러리가 없는 경우: 센서 JPEG 그대로 스트리밍
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_VGA;
    c.fb_count     = 2;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    // PSRAM 없음: 내부 DRAM 에 들어가도록 센서 JPEG(QVGA, 약 10~20KB) 한 장만 사용
    // 얼굴 감지/모자이크는 불가 -> Tools > PSRAM 메뉴를 확인할 것
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_QVGA;
    c.fb_count     = 1;
    c.fb_location  = CAMERA_FB_IN_DRAM;
    Serial.println("[CAM] PSRAM 없음 -> JPEG QVGA, 얼굴 감지 없이 동작");
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK && c.xclk_freq_hz == 20000000) {
    // 일부 보드/센서는 20MHz 에서 실패하므로 10MHz 로 한 번 더 시도
    Serial.printf("[CAM] 초기화 실패 0x%x, XCLK 10MHz 로 재시도\n", err);
    esp_camera_deinit();
    c.xclk_freq_hz = 10000000;
    err = esp_camera_init(&c);
  }
  if (err != ESP_OK) {
    Serial.printf("[CAM] 초기화 실패 0x%x\n", err);
    return false;
  }
  Serial.printf("[CAM] 초기화 완료: %s %s, fb=%d, %s\n",
                c.pixel_format == PIXFORMAT_JPEG ? "JPEG" : "RGB565",
                c.frame_size == FRAMESIZE_VGA ? "VGA" : "QVGA", c.fb_count,
                c.fb_location == CAMERA_FB_IN_PSRAM ? "PSRAM" : "DRAM");
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
    if (CAM_VFLIP)   s->set_vflip(s, 1);
    if (CAM_HMIRROR) s->set_hmirror(s, 1);
  }
  return true;
}

// RGB565(빅엔디안, esp32-camera 형식) 프레임에서 얼굴 박스를 받아 얼굴(또는 사람 전체) 영역을 모자이크 처리
static void mosaicRect(uint8_t *buf, int w, int h, int x1, int y1, int x2, int y2) {
  int bw = x2 - x1, bh = y2 - y1;
  if (bw <= 0 || bh <= 0) return;
#if BLUR_PERSON
  // 얼굴 크기를 기준으로 사람 영역(머리 위 ~ 몸통/다리, 좌우 어깨 폭)으로 확장: 폭 약 4배, 높이 약 7배
  int cx = x1 + bw / 2;
  x1 = cx - bw * 2;        x2 = cx + bw * 2;
  y1 = y1 - (bh * 7) / 10; y2 = y2 + bh * 11 / 2;
#else
  // 머리카락/턱까지 덮이도록 박스를 조금 확장
  x1 -= bw / 5; x2 += bw / 5;
  y1 -= bh / 3; y2 += bh / 5;
#endif
  if (x1 < 0) x1 = 0;  if (y1 < 0) y1 = 0;
  if (x2 > w) x2 = w;  if (y2 > h) y2 = h;
  if (x2 <= x1 || y2 <= y1) return;
  int block = min(x2 - x1, y2 - y1) / 10;   // 영역 크기에 비례한 블록
  if (block < 8) block = 8;

  uint16_t *px = (uint16_t *)buf;
  for (int by = y1; by < y2; by += block) {
    int yEnd = min(by + block, y2);
    for (int bx = x1; bx < x2; bx += block) {
      int xEnd = min(bx + block, x2);
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = by; y < yEnd; y++) {
        for (int x = bx; x < xEnd; x++) {
          uint16_t v = __builtin_bswap16(px[y * w + x]);
          r += (v >> 11) & 0x1F; g += (v >> 5) & 0x3F; b += v & 0x1F;
          n++;
        }
      }
      if (!n) continue;
      uint16_t avg = __builtin_bswap16((uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n)));
      for (int y = by; y < yEnd; y++)
        for (int x = bx; x < xEnd; x++) px[y * w + x] = avg;
    }
  }
}

// 최신 JPEG 프레임을 호출자 버퍼로 복사. lastSeq 와 같은 프레임이면 false
static bool copyLatestFrame(uint8_t **buf, size_t *cap, size_t *len, uint32_t *lastSeq) {
  bool ok = false;
  if (xSemaphoreTake(g_frameMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (g_frame.buf && g_frame.len && g_frame.seq != *lastSeq) {
      if (*cap < g_frame.len) {
        size_t ncap = g_frame.len + 4096;
        uint8_t *nb = (uint8_t *)heap_caps_realloc(*buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) nb = (uint8_t *)realloc(*buf, ncap);
        if (nb) { *buf = nb; *cap = ncap; }
      }
      if (*buf && *cap >= g_frame.len) {
        memcpy(*buf, g_frame.buf, g_frame.len);
        *len = g_frame.len;
        *lastSeq = g_frame.seq;
        ok = true;
      }
    }
    xSemaphoreGive(g_frameMutex);
  }
  return ok;
}

// 카메라 태스크: 캡처 -> 얼굴 감지 -> 모자이크 -> JPEG 인코딩 -> 공유 버퍼 교체
static void cameraTask(void *arg) {
#if FACE_DETECT_AVAILABLE
  static HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
  static HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
#endif
  unsigned long fpsWindow = millis();
  int fpsCount = 0;

  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int faces = 0;
#if FACE_DETECT_AVAILABLE
    if (fb->format == PIXFORMAT_RGB565) {
      std::list<dl::detect::result_t> &cands = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
      std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, cands);
      for (auto &r : results) {
        if (r.box.size() < 4) continue;
        mosaicRect(fb->buf, fb->width, fb->height, r.box[0], r.box[1], r.box[2], r.box[3]);
        faces++;
      }
    }
#endif
    if (faces > 0) g_lastFaceMs = millis();
    g_faces = faces;

    uint8_t *jpg = nullptr;
    size_t jpgLen = 0;
    bool ok;
    if (fb->format == PIXFORMAT_JPEG) {
      // 센서가 이미 JPEG 로 준 경우(PSRAM 없음 / 얼굴 감지 불가): 그대로 복사
      jpg = (uint8_t *)malloc(fb->len);
      ok = (jpg != nullptr);
      if (ok) { memcpy(jpg, fb->buf, fb->len); jpgLen = fb->len; }
    } else {
      ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, STREAM_JPEG_QUALITY, &jpg, &jpgLen);
    }
    esp_camera_fb_return(fb);

    if (ok && jpg) {
      if (xSemaphoreTake(g_frameMutex, portMAX_DELAY) == pdTRUE) {
        if (g_frame.buf) free(g_frame.buf);
        g_frame.buf = jpg;
        g_frame.len = jpgLen;
        g_frame.seq++;
        xSemaphoreGive(g_frameMutex);
      } else {
        free(jpg);
      }
      g_camReady = true;
    } else if (jpg) {
      free(jpg);
    }

    fpsCount++;
    unsigned long now = millis();
    if (now - fpsWindow >= 2000) {
      g_fps = fpsCount * 1000.0f / (now - fpsWindow);
      fpsWindow = now;
      fpsCount = 0;
    }
    vTaskDelay(1);
  }
}

// =====================================================================
// MJPEG 스트림 서버 (포트 81, esp_http_server)
// =====================================================================
static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  uint8_t *buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t seq = 0;
  char hdr[96];

  while (true) {
    if (!copyLatestFrame(&buf, &cap, &len, &seq)) {
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }
    int hl = snprintf(hdr, sizeof(hdr), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    if (httpd_resp_send_chunk(req, hdr, hl) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, (const char *)buf, len) != ESP_OK) break;
  }
  if (buf) free(buf);
  return ESP_OK;
}

static void startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port   = 32769;
  config.stack_size  = 8192;
  config.lru_purge_enable = true;
  httpd_uri_t streamUri = {};
  streamUri.uri     = "/stream";
  streamUri.method  = HTTP_GET;
  streamUri.handler = streamHandler;
  if (httpd_start(&streamHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
    Serial.println("[HTTP] 스트림 서버 시작 (:81/stream)");
  } else {
    Serial.println("[HTTP] 스트림 서버 시작 실패");
  }
}

// =====================================================================
// 서버(server.py) 통신
// =====================================================================
static int serverPost(const String &path, const String &json, String &respBody) {
  if (!serverConfigured() || WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  if (!http.begin(serverBaseUrl() + path)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  respBody = (code > 0) ? http.getString() : String("");
  http.end();
  return code;
}

static String deviceJsonFields() {
  String j = "\"mac\":\"" + g_mac + "\"";
  j += ",\"ip\":\"" + staIp() + "\"";
  j += ",\"room\":\"" + jsonEscape(cfg.room) + "\"";
  j += ",\"ap_ssid\":\"" + jsonEscape(g_apSsid) + "\"";
  return j;
}

// 서버에 기기 등록 (MAC/IP/회의실명). 서버가 인증코드를 발급/보관함
static void serverRegister() {
  g_lastRegister = millis();
  String resp;
  String body = "{" + deviceJsonFields() + ",\"token\":\"" + jsonEscape(cfg.token) + "\"}";
  int code = serverPost("/api/register", body, resp);
  if (code == 200) {
    g_serverOk = true;
    String a = jsonGet(resp, "authorized");
    g_authorized = (a == "true");
    String msg = jsonGet(resp, "message");
    g_serverMsg = msg.length() ? msg : (g_authorized ? "인증됨" : "인증 대기");
    Serial.printf("[SRV] 등록 응답: authorized=%d msg=%s\n", g_authorized, g_serverMsg.c_str());
  } else {
    g_serverOk = false;
    g_authorized = false;
    g_serverMsg = (code < 0) ? "서버에 연결할 수 없음" : ("서버 오류 HTTP " + String(code));
    Serial.printf("[SRV] 등록 실패 (%d)\n", code);
  }
}

static bool serverAuth(const String &authCode, String &message) {
  String resp;
  String body = "{" + deviceJsonFields() + ",\"code\":\"" + jsonEscape(authCode) + "\"}";
  int code = serverPost("/api/auth", body, resp);
  if (code < 0) { message = "서버에 연결할 수 없습니다"; g_serverOk = false; return false; }
  g_serverOk = true;
  if (code == 200 && jsonGet(resp, "ok") == "true") {
    String token = jsonGet(resp, "token");
    if (token.length()) saveToken(token);
    g_authorized = true;
    g_serverMsg = "인증됨";
    message = "인증 성공";
    g_lastHeartbeat = 0;
    g_lastSnapshot = 0;
    return true;
  }
  String err = jsonGet(resp, "error");
  message = err.length() ? err : ("인증 실패 (HTTP " + String(code) + ")");
  return false;
}

static void serverHeartbeat() {
  g_lastHeartbeat = millis();
  String resp;
  String body = "{" + deviceJsonFields();
  body += ",\"token\":\"" + jsonEscape(cfg.token) + "\"";
  body += ",\"occupied\":" + String(occupiedNow() ? "true" : "false");
  body += ",\"faces\":" + String((int)g_faces);
  body += ",\"detect\":" + String(detectAvailable() ? "true" : "false");   // 얼굴 감지(블러) 동작 여부
  body += ",\"rssi\":" + String(WiFi.RSSI());
  body += ",\"fps\":" + String(g_fps, 1);
  body += ",\"uptime\":" + String(millis() / 1000);
  body += "}";
  int code = serverPost("/api/heartbeat", body, resp);
  if (code == 200) {
    g_serverOk = true;
    g_serverMsg = "인증됨";
  } else if (code == 401 || code == 403) {
    g_serverOk = true;
    g_authorized = false;
    g_serverMsg = "인증이 취소되었습니다. 인증코드를 다시 입력하세요";
    saveToken("");
  } else {
    g_serverOk = false;
    g_serverMsg = (code < 0) ? "서버에 연결할 수 없음" : ("서버 오류 HTTP " + String(code));
  }
}

static void serverSnapshot() {
  g_lastSnapshot = millis();
  uint8_t *buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t seq = 0;
  if (!copyLatestFrame(&buf, &cap, &len, &seq)) return;

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (http.begin(serverBaseUrl() + "/api/snapshot")) {
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device-Mac", g_mac);
    http.addHeader("X-Device-Token", cfg.token);
    http.addHeader("X-Device-Room", cfg.room);
    http.addHeader("X-Device-Occupied", occupiedNow() ? "1" : "0");
    http.addHeader("X-Device-Faces", String((int)g_faces));
    http.addHeader("X-Device-Detect", detectAvailable() ? "1" : "0");
    int code = http.POST(buf, len);
    if (code == 401 || code == 403) {
      g_authorized = false;
      g_serverMsg = "인증이 취소되었습니다. 인증코드를 다시 입력하세요";
      saveToken("");
    }
    http.end();
  }
  free(buf);
}

static void manageServerComms() {
  if (WiFi.status() != WL_CONNECTED || !serverConfigured()) return;
  unsigned long now = millis();
  if (!g_authorized) {
    if (g_lastRegister == 0 || now - g_lastRegister >= REGISTER_INTERVAL_MS) serverRegister();
    return;
  }
  if (g_lastHeartbeat == 0 || now - g_lastHeartbeat >= HEARTBEAT_INTERVAL_MS) serverHeartbeat();
  if (g_authorized && g_camReady && (g_lastSnapshot == 0 || now - g_lastSnapshot >= SNAPSHOT_INTERVAL_MS)) serverSnapshot();
}

// =====================================================================
// 웹 페이지 (WiFiManager 의 웹서버에 추가되는 우리 페이지)
// =====================================================================
static const char PAGE_INDEX[] PROGMEM = R"rawliteral(<!DOCTYPE html><html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>회의실 카메라</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--fg:#1d2733;--muted:#6b7683;--line:#dfe3e8;--acc:#2563eb;--ok:#16a34a;--warn:#d97706;--bad:#dc2626}
*{box-sizing:border-box}body{margin:0;font-family:-apple-system,"Apple SD Gothic Neo","Malgun Gothic",sans-serif;background:var(--bg);color:var(--fg)}
header{background:#111827;color:#fff;padding:12px 16px;display:flex;align-items:center;gap:10px}header h1{font-size:17px;margin:0;font-weight:600}
header .sub{opacity:.75;font-size:13px}main{max-width:900px;margin:0 auto;padding:16px;display:grid;gap:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}.card h2{font-size:15px;margin:0 0 10px}
table{border-collapse:collapse;width:100%;font-size:14px}td{padding:5px 4px;border-bottom:1px solid var(--line);vertical-align:top}td:first-child{color:var(--muted);width:120px;white-space:nowrap}
input,button{font:inherit;font-size:14px}input[type=text]{width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:6px;background:#fff}
button{padding:8px 14px;border:none;border-radius:6px;background:var(--acc);color:#fff;cursor:pointer;white-space:nowrap}
button.sec{background:#e5e7eb;color:var(--fg)}button.bad{background:var(--bad)}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.badge{display:inline-block;padding:3px 10px;border-radius:999px;font-size:13px;font-weight:600;color:#fff;background:var(--muted)}
.badge.ok{background:var(--ok)}.badge.warn{background:var(--warn)}.badge.bad{background:var(--bad)}
.video{position:relative;background:#000;border-radius:10px;overflow:hidden;aspect-ratio:4/3;max-height:480px}.video img{width:100%;height:100%;object-fit:contain;display:block}
.video .ov{position:absolute;left:10px;top:10px;display:flex;gap:6px}.msg{margin-top:8px;font-size:13px;min-height:18px}.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
a{color:var(--acc)}.hint{font-size:12px;color:var(--muted)}
</style></head><body>
<header><h1>회의실 카메라</h1><span class="sub" id="hdRoom"></span><span class="sub" style="margin-left:auto">v%FW%</span></header>
<main>
<div class="video"><img id="stream" alt="카메라 스트림"><div class="ov"><span class="badge" id="occBadge">-</span><span class="badge" id="faceBadge">얼굴 0</span></div></div>
<div class="card"><h2>상태</h2><table>
<tr><td>회의실명</td><td id="sRoom">-</td></tr>
<tr><td>WiFi</td><td id="sWifi">-</td></tr>
<tr><td>접속 IP</td><td id="sIp">-</td></tr>
<tr><td>AP</td><td id="sAp">-</td></tr>
<tr><td>MAC</td><td id="sMac">-</td></tr>
<tr><td>서버</td><td id="sServer">-</td></tr>
<tr><td>서버 인증</td><td id="sAuth">-</td></tr>
<tr><td>얼굴 감지</td><td id="sDetect">-</td></tr>
</table></div>
<div class="card" id="authCard"><h2>서버 인증</h2>
<p class="hint">서버 관리자 페이지( http://서버IP:포트/admin )에서 이 기기(MAC)에 발급된 인증코드를 확인해 입력하세요.</p>
<div class="row"><input type="text" id="code" placeholder="인증코드" style="max-width:220px" inputmode="numeric"><button onclick="doAuth()">인증 시도</button></div>
<div class="msg" id="authMsg"></div></div>
<div class="card"><h2>설정</h2><div class="row">
<a href="/wifi"><button class="sec" type="button">WiFi / 서버 / 회의실 설정</button></a>
<a href="/param"><button class="sec" type="button">서버 / 회의실만 변경</button></a>
<a href="/info"><button class="sec" type="button">기기 정보</button></a>
<a href="/capture" target="_blank"><button class="sec" type="button">사진 캡처</button></a>
<button class="sec" onclick="if(confirm('재부팅 할까요?'))fetch('/reboot',{method:'POST'})">재부팅</button>
<button class="bad" onclick="if(confirm('저장된 WiFi/서버 설정을 모두 지우고 재부팅합니다. 계속할까요?'))fetch('/reset',{method:'POST'})">설정 초기화</button>
</div></div>
</main>
<script>
const $=id=>document.getElementById(id);
$('stream').src='http://'+location.hostname+':81/stream';
function badge(el,txt,cls){el.textContent=txt;el.className='badge '+cls}
async function poll(){try{const s=await(await fetch('/status',{cache:'no-store'})).json();
 $('hdRoom').textContent=s.room||'(회의실명 미설정)';$('sRoom').textContent=s.room||'-';
 const wifi={idle:'설정 안됨',connecting:'연결 중...',connected:'연결됨',failed:'연결 실패'}[s.sta]||s.sta;
 $('sWifi').textContent=(s.ssid?s.ssid+' - ':'')+wifi+(s.sta==='connected'?' ('+s.rssi+' dBm)':'');
 $('sIp').innerHTML=s.ip?'<b>'+s.ip+'</b> &nbsp;<a href="http://'+s.ip+'/">http://'+s.ip+'/</a>':'-';
 $('sAp').textContent=s.ap_ssid+' ('+s.ap_ip+')';$('sMac').textContent=s.mac;
 $('sServer').textContent=s.server?s.server+(s.server_ok?' (연결됨)':' (연결 안됨)'):'미설정';
 $('sAuth').innerHTML=(s.authorized?'<span class="badge ok">인증됨</span> ':'<span class="badge warn">미인증</span> ')+(s.server_msg||'');
 $('sDetect').textContent=(s.detect?'사용 중':'사용 불가 (PSRAM 확인)')+' · '+s.fps+' fps';
 if(s.occupied)badge($('occBadge'),'재실','ok');else badge($('occBadge'),'비어있음','');
 $('faceBadge').textContent='얼굴 '+s.faces;
 $('authCard').style.display=s.authorized?'none':'';
}catch(e){}}
setInterval(poll,2000);poll();
async function doAuth(){const m=$('authMsg');m.className='msg';m.textContent='인증 중...';
 try{const r=await fetch('/auth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({code:$('code').value.trim()})});
 const j=await r.json();m.className='msg '+(j.ok?'ok':'bad');m.textContent=j.message||(j.ok?'인증 성공':'인증 실패');if(j.ok)poll();}
 catch(e){m.className='msg bad';m.textContent='요청 실패: '+e}}
</script></body></html>)rawliteral";

// WiFiManager 의 모든 페이지 <head> 에 삽입됨.
//  - /wifisave (저장 직후 페이지) 에서는 /status 를 폴링해서 접속된 IP 를 그 자리에 표시
//  - 페이지 하단에 메인(카메라) 페이지로 가는 링크 추가
static const char WM_HEAD[] PROGMEM = R"rawliteral(<style>body{font-family:-apple-system,"Apple SD Gothic Neo","Malgun Gothic",sans-serif}#ipbox{margin:14px 0;padding:12px;border:1px solid #888;border-radius:8px;font-size:15px;line-height:1.6}</style>
<script>document.addEventListener('DOMContentLoaded',function(){
 var back=document.createElement('p');back.innerHTML='<a href="/">&larr; 카메라 영상 / 상태 페이지</a>';document.body.appendChild(back);
 if(location.pathname!=='/wifisave')return;
 var d=document.createElement('div');d.id='ipbox';d.textContent='WiFi 에 연결하는 중... (최대 30초). 연결 중에 AP 가 잠시 끊기면 다시 AP 에 접속해 이 페이지를 새로고침하세요.';
 document.body.insertBefore(d,back);
 var n=0;
 function chk(){var c=new AbortController();var t=setTimeout(function(){c.abort()},4000);
  fetch('/status',{cache:'no-store',signal:c.signal}).then(function(r){return r.json()}).then(function(s){clearTimeout(t);
   if(s.sta==='connected'){d.innerHTML='<b>'+s.ssid+'</b> 에 연결되었습니다.<br>접속된 IP: <b style="font-size:22px">'+s.ip+'</b><br>같은 WiFi 에서 <a href="http://'+s.ip+'/">http://'+s.ip+'/</a> 로 접속하면 카메라 영상이 보입니다.'+(s.hostname?'<br>mDNS: <a href="http://'+s.hostname+'.local/">http://'+s.hostname+'.local/</a>':'')+'<br><a href="/">AP 에서 계속 보기</a>';return}
   if(s.sta==='failed'){d.innerHTML='연결 실패: SSID 와 비밀번호를 확인하세요. <a href="/wifi">다시 설정</a>';return}
   setTimeout(chk,2000)}).catch(function(){clearTimeout(t);if(++n<40)setTimeout(chk,2000)})}
 setTimeout(chk,1500)})</script>)rawliteral";

static void noCache() {
  web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  web.sendHeader("Pragma", "no-cache");
  web.sendHeader("Expires", "0");
}

static void handleRoot() {
  String page = FPSTR(PAGE_INDEX);
  page.replace("%FW%", FW_VERSION);
  noCache();
  web.send(200, "text/html; charset=utf-8", page);
}

static void handleStatus() {
  const char *sta = "idle";
  if (WiFi.status() == WL_CONNECTED) sta = "connected";
  else if (wm.getWiFiIsSaved()) {
    wl_status_t r = (wl_status_t)wm.getLastConxResult();
    sta = (r == WL_CONNECT_FAILED || r == WL_NO_SSID_AVAIL) ? "failed" : "connecting";
  }
  String j = "{";
  j += "\"mac\":\"" + g_mac + "\"";
  j += ",\"hostname\":\"" + g_hostname + "\"";
  j += ",\"ap_ssid\":\"" + jsonEscape(g_apSsid) + "\"";
  j += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"sta\":\"" + String(sta) + "\"";
  j += ",\"ssid\":\"" + jsonEscape(wm.getWiFiSSID()) + "\"";
  j += ",\"ip\":\"" + staIp() + "\"";
  j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  j += ",\"server\":\"" + (serverConfigured() ? jsonEscape(cfg.serverHost + ":" + String(cfg.serverPort)) : String("")) + "\"";
  j += ",\"server_ok\":" + String(g_serverOk ? "true" : "false");
  j += ",\"server_msg\":\"" + jsonEscape(g_serverMsg) + "\"";
  j += ",\"authorized\":" + String(g_authorized ? "true" : "false");
  j += ",\"room\":\"" + jsonEscape(cfg.room) + "\"";
  j += ",\"occupied\":" + String(occupiedNow() ? "true" : "false");
  j += ",\"faces\":" + String((int)g_faces);
  j += ",\"detect\":" + String(detectAvailable() ? "true" : "false");
  j += ",\"fps\":" + String(g_fps, 1);
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"fw\":\"" FW_VERSION "\"";
  j += "}";
  noCache();
  web.send(200, "application/json; charset=utf-8", j);
}

static void handleAuth() {
  String code = web.arg("code");
  code.trim();
  if (code.length() == 0) { web.send(400, "application/json", "{\"ok\":false,\"message\":\"인증코드를 입력하세요\"}"); return; }
  if (WiFi.status() != WL_CONNECTED) { web.send(200, "application/json", "{\"ok\":false,\"message\":\"WiFi 가 연결되어 있지 않습니다\"}"); return; }
  if (!serverConfigured()) { web.send(200, "application/json", "{\"ok\":false,\"message\":\"서버 IP 가 설정되어 있지 않습니다\"}"); return; }
  String msg;
  bool ok = serverAuth(code, msg);
  web.send(200, "application/json; charset=utf-8", String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscape(msg) + "\"}");
}

static void handleCapture() {
  uint8_t *buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t seq = 0;
  if (!copyLatestFrame(&buf, &cap, &len, &seq)) { web.send(503, "text/plain", "no frame"); return; }
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.sendHeader("Cache-Control", "no-store");
  web.setContentLength(len);
  web.send(200, "image/jpeg", "");
  web.client().write(buf, len);
  free(buf);
}

static void handleReboot() {
  web.send(200, "text/plain; charset=utf-8", "재부팅합니다");
  delay(300);
  ESP.restart();
}

static void handleReset() {
  prefs.begin("meetcam", false);
  prefs.clear();
  prefs.end();
  wm.resetSettings();           // 저장된 WiFi 자격증명 삭제
  web.send(200, "text/plain; charset=utf-8", "설정을 초기화하고 재부팅합니다");
  delay(300);
  ESP.restart();
}

static void handleSetupRedirect() {
  web.sendHeader("Location", "/wifi", true);
  web.send(302, "text/plain", "");
}

// WiFiManager 가 웹서버를 만든 직후(자기 라우트를 등록하기 전) 호출됨 -> 먼저 등록한 라우트가 우선
static void bindRoutes() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/status", HTTP_GET, handleStatus);
  web.on("/auth", HTTP_POST, handleAuth);
  web.on("/capture", HTTP_GET, handleCapture);
  web.on("/reboot", HTTP_POST, handleReboot);
  web.on("/reset", HTTP_POST, handleReset);
  web.on("/setup", HTTP_GET, handleSetupRedirect);
  Serial.println("[HTTP] 웹 라우트 등록");
}

// =====================================================================
// WiFiManager 연동
// =====================================================================
// 포털에서 [Save] 를 누르면 호출됨: 서버 IP / 포트 / 회의실명 저장
static void onSaveParams() {
  String host = pServer->getValue(); host.trim();
  String room = pRoom->getValue();   room.trim();
  int port = String(pPort->getValue()).toInt();
  if (port <= 0 || port > 65535) port = DEFAULT_SERVER_PORT;
  bool changed = (host != cfg.serverHost) || (port != cfg.serverPort) || (room != cfg.room);
  cfg.serverHost = host;
  cfg.serverPort = port;
  cfg.room       = room;
  if (changed) { cfg.token = ""; g_authorized = false; }   // 서버/회의실이 바뀌면 재인증
  saveConfig();
  g_lastRegister = 0;
  Serial.printf("[CFG] 저장: server=%s:%d room=%s\n", cfg.serverHost.c_str(), cfg.serverPort, cfg.room.c_str());
}

static void onStaConnected() {
  g_wasConnected = true;
  Serial.printf("[STA] 연결됨 SSID=%s IP=%s RSSI=%d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  MDNS.end();
  if (MDNS.begin(g_hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[STA] mDNS: http://%s.local/\n", g_hostname.c_str());
  }
  g_lastRegister = 0;   // 즉시 서버 등록
}

static void trackSta() {
  bool c = (WiFi.status() == WL_CONNECTED);
  if (c && !g_wasConnected) onStaConnected();
  if (!c && g_wasConnected) { g_wasConnected = false; Serial.println("[STA] 연결 끊김"); }
}

// 설정 포털(AP + 웹)이 어떤 이유로든 내려가 있으면 다시 띄운다 (KEEP_AP_AFTER_CONNECT)
static void keepPortalAlive() {
  if (!KEEP_AP_AFTER_CONNECT) return;
  if (wm.getConfigPortalActive() || wm.getWebPortalActive()) return;
  if (millis() - g_lastPortalTry < 10000) return;
  g_lastPortalTry = millis();
  Serial.println("[WM] 설정 포털 재시작");
  wm.startConfigPortal(g_apSsid.c_str(), apPassword());
}

static void setupWiFiManager() {
  wm.setDebugOutput(true);                       // 스캔 결과 등이 시리얼에 찍힘
  wm.setTitle("회의실 카메라 " FW_VERSION);
  wm.setHostname(g_hostname.c_str());
  wm.setConfigPortalBlocking(false);             // loop() 에서 wm.process() 로 처리
  wm.setConfigPortalTimeout(0);                  // 포털 자동 종료 없음
  wm.setDisableConfigPortal(!KEEP_AP_AFTER_CONNECT);   // false: 접속 성공 후에도 포털/AP 유지
  wm.setConnectTimeout(STA_CONNECT_TIMEOUT_S);
  wm.setConnectRetries(2);
  wm.setWiFiAutoReconnect(true);
  wm.setScanDispPerc(true);                      // 신호를 % 로 표시
  wm.setRemoveDuplicateAPs(true);
  wm.setMinimumSignalQuality(8);
  wm.setShowInfoErase(true);
  wm.setCustomHeadElement(WM_HEAD);
  std::vector<const char *> menu = {"wifi", "param", "info", "restart", "erase"};
  wm.setMenu(menu);

  // SSID/비밀번호 아래에 붙는 입력란 (기본으로 /wifi 페이지에 함께 표시됨)
  static char portBuf[8];
  snprintf(portBuf, sizeof(portBuf), "%d", cfg.serverPort);
  pServer = new WiFiManagerParameter("server", "서버 IP (server.py 가 실행되는 PC)", cfg.serverHost.c_str(), 40);
  pPort   = new WiFiManagerParameter("port",   "서버 포트", portBuf, 6);
  pRoom   = new WiFiManagerParameter("room",   "회의실명 (예: 3층 대회의실)", cfg.room.c_str(), 40);
  wm.addParameter(pServer);
  wm.addParameter(pPort);
  wm.addParameter(pRoom);
  wm.setSaveParamsCallback(onSaveParams);   // /wifisave, /paramsave 모두에서 호출됨
  wm.setSaveConfigCallback(onSaveParams);   // (WiFi 저장 후 접속 성공 시에도 한 번 더 - 중복 호출 무해)
  wm.setWebServerCallback(bindRoutes);

  // 저장된 WiFi 가 있으면 접속 시도(최대 STA_CONNECT_TIMEOUT_S 초 대기), 실패/미설정이면 포털 시작
  bool connected = wm.autoConnect(g_apSsid.c_str(), apPassword());
  if (connected) {
    Serial.println("[WM] 저장된 WiFi 에 접속됨");
    if (KEEP_AP_AFTER_CONNECT) wm.startConfigPortal(g_apSsid.c_str(), apPassword());   // AP + 설정 포털도 계속 제공
    else                       wm.startWebPortal();                                     // 웹 페이지만 (STA IP)
  } else {
    Serial.printf("[WM] 설정 포털 시작: AP '%s' 에 접속 후 http://192.168.4.1/ -> [WiFi 설정]\n", g_apSsid.c_str());
  }
}

// =====================================================================
// setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32-S3-CAM 회의실 재실 감지 v" FW_VERSION " ===");
  Serial.printf("PSRAM: %s (%u bytes)\n", psramFound() ? "OK" : "없음", (unsigned)ESP.getPsramSize());
  if (!psramFound()) {
    Serial.println("  !! PSRAM 이 초기화되지 않았습니다. Arduino IDE: Tools > PSRAM 에서");
    Serial.println("     ESP32-S3-WROOM-1 N8R8 / N16R8 (모듈 라벨 끝이 R8) -> \"OPI PSRAM\"");
    Serial.println("     ESP32-S3-WROOM-1 N4R2 / N8R2 (라벨 끝이 R2)      -> \"QSPI PSRAM\"");
    Serial.println("     라벨에 R 이 없으면(N8, N16) PSRAM 이 없는 모듈이라 얼굴 감지는 불가합니다.");
  }
  Serial.printf("얼굴 감지: %s\n", detectAvailable() ? "사용" : "사용 불가");

  g_frameMutex = xSemaphoreCreateMutex();
  loadConfig();

  // 기기 고유 ID: eFuse 에 새겨진 STA MAC. (WiFi.macAddress() 는 드라이버가 뜨기 전에 호출하면
  // 00:00:00:00:00:00 을 돌려줄 수 있어 모든 보드가 같은 이름이 되는 문제가 있음)
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18], suffix[5];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  g_mac      = macStr;
  g_apSsid   = String(AP_SSID_PREFIX) + suffix;
  g_hostname = String("meetingcam-") + suffix;
  g_hostname.toLowerCase();
  Serial.printf("기기 MAC: %s  ->  AP: %s / 호스트명: %s\n", g_mac.c_str(), g_apSsid.c_str(), g_hostname.c_str());
  if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 0)
    Serial.println("  !! MAC 이 0 입니다. 보드 패키지/eFuse 문제 - 시리얼 출력을 알려주세요");
  WiFi.mode(WIFI_STA);

  if (initCamera()) {
    xTaskCreatePinnedToCore(cameraTask, "cam", 16384, nullptr, 1, nullptr, 1);
    startStreamServer();
  } else {
    Serial.println("[CAM] 카메라 없이 계속 진행합니다 (핀 설정 확인)");
  }

  setupWiFiManager();
}

void loop() {
  wm.process();          // 설정 포털(DNS/웹) + 저장 후 접속 처리
  trackSta();
  keepPortalAlive();
  manageServerComms();
  delay(2);
}
