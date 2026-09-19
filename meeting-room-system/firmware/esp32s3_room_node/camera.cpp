#include "camera.h"
#include "config.h"
#include "camera_pins.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static bool              s_ready = false;
static httpd_handle_t    s_camHttpd = NULL;
static SemaphoreHandle_t s_camMutex = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

bool cameraReady() { return s_ready; }

camera_fb_t *cameraGrab(uint32_t timeoutMs) {
  if (!s_ready || !s_camMutex) return nullptr;
  if (xSemaphoreTake(s_camMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return nullptr;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(s_camMutex);
    return nullptr;
  }
  return fb;
}

void cameraRelease(camera_fb_t *fb) {
  if (fb) esp_camera_fb_return(fb);
  if (s_camMutex) xSemaphoreGive(s_camMutex);
}

bool cameraBegin() {
  s_camMutex = xSemaphoreCreateMutex();

  camera_config_t config;
  memset(&config, 0, sizeof(config));
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;   // 800x600: 스트리밍과 화질의 균형
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] 초기화 실패 (0x%x). 핀맵/보드 설정을 확인하세요.\n", err);
    s_ready = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    if (s->id.PID == OV2640_PID) {
      s->set_vflip(s, 0);
      s->set_hmirror(s, 0);
      s->set_brightness(s, 1);
      s->set_saturation(s, 0);
    }
  }

  s_ready = true;
  Serial.println("[CAM] 카메라 준비 완료");
  return true;
}

void cameraSetFrameSize(framesize_t fs) {
  if (!s_ready) return;
  sensor_t *s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, fs);
}

// --- HTTP 핸들러 (포트 81) -------------------------------------------------

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = cameraGrab();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  cameraRelease(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "12");

  char part_buf[72];
  while (true) {
    camera_fb_t *fb = cameraGrab();
    if (!fb) { res = ESP_FAIL; break; }

    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));

    cameraRelease(fb);
    if (res != ESP_OK) break;      // 클라이언트가 창을 닫으면 여기서 빠져나온다
    vTaskDelay(pdMS_TO_TICKS(40)); // 다른 태스크(웹/센서)에 시간 양보
  }
  return res;
}

static esp_err_t root81_handler(httpd_req_t *req) {
  static const char *html =
      "<!doctype html><meta charset='utf-8'><title>Room Cam</title>"
      "<body style='margin:0;background:#111'>"
      "<img src='/stream' style='width:100%;height:auto'>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void cameraStartServer() {
  if (!s_ready || s_camHttpd) return;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port    = CAMERA_HTTP_PORT;
  config.ctrl_port      = CAMERA_HTTP_PORT + 1000;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;
  config.stack_size     = 8192;

  httpd_uri_t stream_uri  = {"/stream",  HTTP_GET, stream_handler,  NULL};
  httpd_uri_t capture_uri = {"/capture", HTTP_GET, capture_handler, NULL};
  httpd_uri_t root_uri    = {"/",        HTTP_GET, root81_handler,  NULL};

  if (httpd_start(&s_camHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(s_camHttpd, &stream_uri);
    httpd_register_uri_handler(s_camHttpd, &capture_uri);
    httpd_register_uri_handler(s_camHttpd, &root_uri);
    Serial.printf("[CAM] 스트림 서버 시작: 포트 %d\n", CAMERA_HTTP_PORT);
  } else {
    Serial.println("[CAM] 스트림 서버 시작 실패");
    s_camHttpd = NULL;
  }
}

void cameraStopServer() {
  if (s_camHttpd) {
    httpd_stop(s_camHttpd);
    s_camHttpd = NULL;
  }
}
