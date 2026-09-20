# ESP32-S3-CAM 회의실 예약 / 재실 감지 시스템

```
[ESP32-S3-CAM]  --(얼굴 감지 → 재실 상태 + 블러 스냅샷)-->  [server.py (Flask)]  <--(예약/현황)--  [브라우저]
   :80  설정/영상/인증 페이지
   :81  MJPEG 스트림 (얼굴 모자이크)
```

## 파일

| 파일 | 설명 |
|---|---|
| `esp32-s3-cam-meetingroom.ino` | 보드 펌웨어 (Arduino IDE) |
| `server.py` | 예약/재실 서버 (Flask) |
| `clients.csv` | 서버가 자동 생성 – 클라이언트 MAC / IP / 회의실명 / 인증코드 |
| `reservations.csv` | 서버가 자동 생성 – 예약 내역 |
| `rooms.json` | 서버가 자동 생성 – 층 목록 + 회의실 기본 정보(층/TV/의자/테이블/화이트보드/화상회의 장비/비고), ⚙ 설정 탭에서 편집 |
| `snapshots/` | 서버가 자동 생성 – 기기별 최신 스냅샷 (블러 처리됨) |
| `models/` | 서버측 검출 모델: 얼굴 YuNet(232KB) + 사람 YOLOX(36MB) |

## 1. 서버 실행

```bash
pip install flask opencv-python-headless numpy   # opencv/numpy 는 서버측 얼굴 블러용(선택)
python server.py                      # http://<서버IP>:5000
PORT=8080 ADMIN_PASSWORD=비밀번호 python server.py   # 포트 변경 / 관리자 페이지 보호
```

- `/` : 대시보드 — 상단 탭 [전체][층별…][⚙ 설정]. 층 탭: 왼쪽 카메라 카드 | 가운데 회의실 기본 정보 | 오른쪽 이 날의 예약 목록, 아래에 예약 시간 막대(08:00~20:00, 30분 단위)
  - 예약 막대: 가운데 끌기=이동, 양끝 끌기=시간 조절, 더블클릭=수정 창, ✕=취소. 지난 예약은 회색
  - ⚙ 설정 탭: 층 추가/삭제, 회의실별 층·TV·의자 수·테이블 수·화이트보드·화상회의 장비·비고 저장
- `/admin` : 접속한 클라이언트 목록, **인증코드 확인**, 회의실명 확인/수정, 인증 취소

서버측 블러(안전장치): 보드에서 얼굴 감지가 꺼져 있거나(PSRAM 없음) 놓친 사람이 있어도, 서버가 받은 스냅샷에서
사람을 찾아 **사람 전체**를 한 번 더 모자이크합니다 (얼굴: YuNet, 사람: YOLOX — OpenCV DNN). 서버가 사람을 찾으면
보드 감지가 꺼져 있어도 30초간 "재실"로 표시합니다. 모델 파일은 `models/` 에 동봉(없으면 시작 시 자동 다운로드).
- `SERVER_BLUR=0 python server.py` : 서버측 블러 끄기
- `BLUR_MODE=face python server.py` : 사람 전체 대신 얼굴만 모자이크

## 2. 펌웨어 업로드 (Arduino IDE)

1. 보드 매니저에서 **esp32 (Espressif)** 3.x 설치
2. 라이브러리 매니저에서 **WiFiManager (by tzapu)** 2.0.17 이상 설치
3. `esp32-s3-cam-meetingroom.ino` 상단에서 카메라 핀 모델 선택
   (`CAMERA_MODEL_ESP32S3_EYE` = Freenove / 대부분의 ESP32-S3-CAM, `CAMERA_MODEL_XIAO_ESP32S3` 등)
4. 툴 메뉴 설정
   - Board: **ESP32S3 Dev Module**
   - PSRAM: 모듈 라벨에 따라 선택 ← 얼굴 감지에 필수. 시리얼에 `PSRAM: 없음` 이 나오면 이 설정이 틀린 것
     - `ESP32-S3-WROOM-1-N8R8`, `N16R8` (끝이 **R8**) → **OPI PSRAM** (대부분의 S3-CAM 보드)
     - `N4R2`, `N8R2` (끝이 **R2**) → **QSPI PSRAM**
     - `N8`, `N16` (R 없음) → PSRAM 없음. 영상만 나오고 얼굴 감지/모자이크는 동작하지 않음
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
   - Flash Size: 보드에 맞게 (8MB/16MB)
   - USB CDC On Boot: Enabled
5. 업로드

## 3. 사용 순서

1. 보드 부팅 → WiFi 목록에 `MeetingCam-XXXX` (비밀번호 `12345678`) 가 보임 → 접속
2. 브라우저에서 `http://192.168.4.1/` → **WiFi / 서버 / 회의실 설정** (= `/wifi`, WiFiManager 설정 포털)
   - 스캔된 SSID 목록에서 클릭해 선택 + 비밀번호 입력
   - 그 아래 서버 IP / 포트 / 회의실명 입력 → **Save**
   - 연결되면 같은 화면에 **접속된 IP** 가 표시됨 (AP 는 계속 유지되어 재설정 가능)
3. 서버 콘솔 또는 `http://<서버IP>:5000/admin` 에서 해당 MAC 의 **인증코드** 확인
4. `http://<보드IP>/` 접속 → 얼굴이 모자이크된 영상 확인 → 하단 **서버 인증** 란에 코드 입력 → **인증 시도**
5. 인증되면 서버 대시보드에 회의실 카드(재실 여부 / 스냅샷)가 나타나고 예약 가능

## 보드 웹 주소

| 주소 | 설명 |
|---|---|
| `http://<ip>/` | 메인 (영상 + 상태 + 인증) |
| `http://<ip>/wifi` | WiFi / 서버 / 회의실명 설정 (WiFiManager) |
| `http://<ip>/param` | 서버 / 회의실명만 변경 |
| `http://<ip>/info` | 기기 정보 |
| `http://<ip>:81/stream` | MJPEG 스트림 (얼굴 모자이크) |
| `http://<ip>/capture` | 정지 사진 |
| `http://<ip>/status` | 상태 JSON |

## 참고

- 얼굴 감지는 ESP32-S3 + PSRAM 에서만 동작합니다 (PSRAM 이 꺼져 있으면 영상만 나오고 재실 감지는 안 됨).
- 재실 판정: 얼굴이 감지된 뒤 30초(`PRESENCE_HOLD_MS`) 동안 "재실" 유지.
- 포트 81 스트림은 한 번에 한 명만 볼 수 있습니다. 서버 대시보드는 3초마다 올라오는 스냅샷을 표시합니다.
- 서버와 보드가 같은 네트워크(같은 공유기)에 있어야 합니다.
