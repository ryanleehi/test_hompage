/*
 * config.h - 회의실 노드(ESP32-S3-CAM + PIR) 공통 설정
 *
 * 대부분의 값은 Wi-Fi 설정 포털(브라우저)에서 바꿀 수 있고,
 * 여기 값들은 "공장 초기값" 역할을 한다.
 */
#pragma once

// ---------------------------------------------------------------------------
// 1) 카메라 보드 선택 (하나만 주석 해제)
// ---------------------------------------------------------------------------
#define CAMERA_MODEL_FREENOVE_ESP32S3   // Freenove ESP32-S3-WROOM CAM (가장 흔한 S3-CAM)
// #define CAMERA_MODEL_ESP32S3_EYE     // Espressif ESP32-S3-EYE (핀맵 동일)
// #define CAMERA_MODEL_XIAO_ESP32S3    // Seeed XIAO ESP32S3 Sense
// #define CAMERA_MODEL_CUSTOM          // camera_pins.h 하단에서 직접 정의

// ---------------------------------------------------------------------------
// 2) 하드웨어 핀
// ---------------------------------------------------------------------------
#define PIR_PIN              21    // HC-SR501 / AM312 OUT 핀
#define PIR_ACTIVE_HIGH       1    // 사람 감지 시 HIGH면 1, LOW면 0
#define STATUS_LED_PIN        2    // 보드 LED (없으면 -1)
#define STATUS_LED_ACTIVE_HIGH 1
#define FLASH_LED_PIN        -1    // 플래시 LED (없으면 -1)
#define RESET_BUTTON_PIN      0    // BOOT 버튼: 3초 이상 누르면 Wi-Fi 설정 초기화

// ---------------------------------------------------------------------------
// 3) 재실(occupancy) 판정
// ---------------------------------------------------------------------------
#define PIR_WARMUP_MS            30000UL   // 전원 인가 후 PIR 안정화 대기(무시 구간)
#define OCCUPANCY_HOLD_MS       120000UL   // 마지막 감지 후 이 시간까지는 "사용중" 유지
#define MOTION_DEBOUNCE_MS        1500UL   // 같은 움직임을 중복 카운트하지 않는 간격

// ---------------------------------------------------------------------------
// 4) 기본 회의실 정보 (포털에서 수정 가능)
// ---------------------------------------------------------------------------
#define DEFAULT_ROOM_ID      "room-1"          // 중앙 서버에서 이 회의실을 구분하는 키
#define DEFAULT_ROOM_NAME    "회의실 1"
#define DEFAULT_SERVER_URL   ""                // 예: http://meeting.example.com  (비우면 단독 동작)
#define DEFAULT_DEVICE_TOKEN "change-me-token" // 서버의 DEVICE_TOKEN 과 동일해야 함

// ---------------------------------------------------------------------------
// 5) Wi-Fi 설정 포털(AP 모드)
// ---------------------------------------------------------------------------
#define AP_SSID_PREFIX       "RoomCam-"   // 실제 SSID = RoomCam-<칩ID 4자리>
#define AP_PASSWORD          "12345678"   // 8자 이상. 비우면 개방형 AP
#define STA_CONNECT_TIMEOUT_MS 20000UL    // 저장된 AP 접속 시도 시간
#define PORTAL_IDLE_REBOOT_MS 600000UL    // 설정 포털에서 10분간 아무 일 없으면 재부팅

// ---------------------------------------------------------------------------
// 6) 시간대 / NTP
// ---------------------------------------------------------------------------
#define TZ_INFO              "KST-9"      // 한국 표준시 (POSIX TZ 문자열)
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"

// ---------------------------------------------------------------------------
// 7) 중앙 서버 동기화 주기
// ---------------------------------------------------------------------------
#define HEARTBEAT_INTERVAL_MS   15000UL   // 재실 상태 업로드 주기
#define PULL_INTERVAL_MS        60000UL   // 예약 목록 내려받기 주기
#define SNAPSHOT_INTERVAL_MS    30000UL   // 대시보드용 스냅샷 업로드 주기 (0이면 사용 안 함)
#define HTTP_TIMEOUT_MS          8000UL

// ---------------------------------------------------------------------------
// 8) 로컬 저장 한도
// ---------------------------------------------------------------------------
#define MAX_RESERVATIONS        24        // 기기에 보관하는 예약 최대 개수
#define QUICK_BOOK_MINUTES      30        // "지금 바로 사용" 버튼의 기본 사용 시간

#define FIRMWARE_VERSION     "1.0.0"
#define CAMERA_HTTP_PORT     81           // MJPEG 스트림 / 스냅샷 포트
