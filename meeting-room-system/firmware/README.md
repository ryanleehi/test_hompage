# 펌웨어 (ESP32-S3-CAM + PIR)

Arduino IDE에서 `esp32s3_room_node/esp32s3_room_node.ino` 를 열면 나머지 파일이 자동으로 함께 열립니다.
**추가 라이브러리 설치가 필요 없습니다.** (esp32 보드 패키지 3.x 에 포함된 것만 사용)

## 파일 구성

| 파일 | 역할 |
|---|---|
| `esp32s3_room_node.ino` | 부팅 흐름, Wi-Fi 모드 전환, 웹 라우트 |
| `config.h` | 핀/시간/주기 등 모든 설정값 (**여기부터 확인**) |
| `camera_pins.h` | 보드별 카메라 핀맵 |
| `settings.*` | NVS에 저장되는 사용자 설정 (SSID, 회의실 정보, 서버 주소) |
| `camera.*` | OV2640 초기화, 포트 81 MJPEG 스트림/스냅샷 |
| `occupancy.*` | PIR 샘플링과 재실 판정 |
| `reservations.*` | 기기에 저장되는 예약 목록 (NVS) |
| `timeutil.*` | NTP 시각, 날짜/시간 문자열 변환 |
| `cloud.*` | 중앙 서버 등록/하트비트/예약 동기화/스냅샷 업로드 |
| `pages.h` | 설정 포털과 회의실 페이지 HTML (PROGMEM) |

## 보드 설정 (Arduino IDE → 도구)

| 항목 | 값 |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | OPI PSRAM |
| Flash Size | 8MB (보드에 맞게) |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |

PSRAM을 켜지 않으면 카메라 해상도가 VGA로 낮아지고 스트리밍이 끊길 수 있습니다.

## 동작 요약

```
부팅
 ├─ 저장된 Wi-Fi 있음 → 20초간 접속 시도
 │    ├─ 성공 → STA 모드
 │    │         · http://<IP>/       회의실 페이지 (예약)
 │    │         · http://<IP>:81/stream  실시간 영상
 │    │         · 중앙 서버와 동기화 시작
 │    └─ 실패 → 설정 포털
 └─ 저장된 Wi-Fi 없음 → 설정 포털
                        · AP "RoomCam-XXXX" + 캡티브 포털
                        · SSID 선택/비밀번호 입력 → 접속되면 할당 IP 표시
                        · 20초 뒤 STA 모드로 재부팅
```

- **BOOT 버튼 3초 이상** 누르면 Wi-Fi 설정만 지우고 재부팅합니다.
- LED(GPIO 2) 상태: 빠른 점멸 = 설정 모드, 느린 점멸 = 재접속 중, 상시 점등 = 사용중.
- 중앙 서버가 꺼져 있어도 기기 단독으로 예약을 받아 두었다가 서버가 살아나면 올립니다.
- 서버에 이미 겹치는 예약이 있으면 그 자리에서 거절 사유를 보여줍니다.

## 자주 고치게 되는 설정 (`config.h`)

| 상수 | 기본값 | 의미 |
|---|---|---|
| `PIR_PIN` | 21 | PIR OUT 핀 |
| `OCCUPANCY_HOLD_MS` | 120000 | 마지막 감지 후 "사용중" 유지 시간 |
| `PIR_WARMUP_MS` | 30000 | 전원 인가 후 PIR 무시 구간 |
| `TZ_INFO` | `KST-9` | 시간대 |
| `SNAPSHOT_INTERVAL_MS` | 30000 | 대시보드용 스냅샷 업로드 주기 (0이면 끔) |
| `QUICK_BOOK_MINUTES` | 30 | "지금 바로 사용" 기본 시간 |
| `AP_PASSWORD` | `12345678` | 설정 포털 AP 비밀번호 |
