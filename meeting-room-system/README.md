# 회의실 재실 감지 · 예약 관리 시스템

ESP32-S3-CAM(OV2640) + PIR 센서를 회의실마다 한 대씩 설치해서

- **회의실에 사람이 있는지** (PIR 재실 감지)
- **회의실이 예약되어 있는지** (예약 현황)
- **회의실 안이 어떤 상태인지** (카메라 영상)

를 한 화면에서 보고, 그 자리에서 **예약까지** 할 수 있게 만든 시스템입니다.

```
  ┌──────────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐
  │ 회의실 1             │   │ 회의실 2             │   │ 회의실 N             │
  │ ESP32-S3-CAM + PIR   │   │ ESP32-S3-CAM + PIR   │   │ ESP32-S3-CAM + PIR   │
  │ http://192.168.0.21  │   │ http://192.168.0.22  │   │ http://192.168.0.2N  │
  └──────────┬───────────┘   └──────────┬───────────┘   └──────────┬───────────┘
             │  재실 상태 / 스냅샷 업로드, 예약 동기화 (HTTP)        │
             └──────────────────────┬───────────────────────────────┘
                                    ▼
                    ┌───────────────────────────────┐
                    │  통합 서버 (Node.js, 무의존)   │
                    │  http://meeting.example.com   │
                    └───────────────┬───────────────┘
                                    ▼
                    ┌───────────────────────────────┐
                    │  통합 대시보드 (브라우저)      │
                    │  모든 회의실 상태 + 예약       │
                    └───────────────────────────────┘
```

---

## 1. 구성

| 폴더 | 내용 |
|---|---|
| `firmware/esp32s3_room_node/` | Arduino IDE용 ESP32-S3 펌웨어 (**추가 라이브러리 없음**) |
| `server/` | 통합 서버 (Node.js 18+, **npm 의존성 없음**) |
| `server/public/` | 통합 대시보드 클라이언트 (프레임워크 없는 순수 HTML/CSS/JS) |
| `server/tools/simulator.js` | 하드웨어 없이 서버/대시보드를 시험하는 기기 시뮬레이터 |
| `docs/` | 배선도, API 명세 |

---

## 2. 빠른 시작

### 2-1. 서버 먼저 띄우기

```bash
cd meeting-room-system/server
DEVICE_TOKEN=원하는토큰 TZ=Asia/Seoul node index.js
# → http://localhost:3000
```

npm install 이 필요 없습니다. Node 18 이상이면 바로 실행됩니다.

하드웨어가 아직 없다면 시뮬레이터로 화면을 먼저 확인할 수 있습니다.

```bash
node tools/simulator.js --rooms 4 --token 원하는토큰
```

### 2-2. 펌웨어 올리기

1. Arduino IDE → **보드매니저**에서 `esp32` by Espressif **3.x** 설치
2. `firmware/esp32s3_room_node/esp32s3_room_node.ino` 열기
3. `config.h` 에서 보드 모델과 PIR 핀만 확인 (기본값: Freenove ESP32-S3 CAM, PIR = GPIO 21)
4. 보드 설정
   - Board: **ESP32S3 Dev Module**
   - PSRAM: **OPI PSRAM** (보드에 맞게. Freenove/S3-EYE는 OPI)
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
   - USB CDC On Boot: **Enabled** (시리얼 모니터를 쓰려면)
5. 업로드

### 2-3. Wi-Fi 설정 (설정 포털)

1. 전원을 넣으면 **`RoomCam-XXXX`** 라는 Wi-Fi가 생깁니다. (기본 비밀번호 `12345678`)
2. 휴대폰/노트북으로 접속하면 설정 화면이 자동으로 뜹니다.
   (안 뜨면 브라우저에서 `http://192.168.4.1` )
3. 화면에서
   - 주변 **SSID 목록** 중 접속할 공유기를 고르고 **비밀번호** 입력
   - **회의실 ID / 이름**, **중앙 서버 주소**, **기기 토큰** 입력
4. **저장하고 접속** 을 누르면 STA 모드로 접속을 시도하고,
   접속에 성공하면 **그 화면에 할당받은 IP 주소가 크게 표시**됩니다.
5. 20초 뒤 기기는 STA 모드로 재부팅합니다. 같은 네트워크에서 그 IP로 접속하면
   회의실 영상과 예약 화면이 나옵니다.

> Wi-Fi를 다시 설정하려면 **BOOT 버튼을 3초 이상** 누르거나,
> 회의실 페이지 하단의 **Wi-Fi 재설정** 버튼을 누르면 됩니다.

---

## 3. 화면

### 기기 화면 (`http://<할당받은 IP>/`)

- 실시간 MJPEG 영상 (`http://<IP>:81/stream`) / 스냅샷
- 지금 사용중인지, 마지막 움직임이 언제였는지
- 오늘 예약 목록과 진행 상태
- 예약 등록 / 취소, **지금 바로 사용**(기본 30분) 버튼
- 기기 정보(IP, 신호세기, 서버 연결 상태, 펌웨어)

### 통합 대시보드 (`http://<서버 주소>/`)

- 전체 / 사용중 / 비어있음 / 온라인 기기 / 확인 필요 요약
- 회의실 카드: 최신 스냅샷, 상태 배지, 하루 타임라인, 현재·다음 예약
- 카드에서 바로 예약, 상세 모달에서 라이브 영상 열기 및 예약 취소
- 최근 활동 로그(입실 감지, 노쇼 자동 해제, 예약/취소)

상태 배지는 다섯 가지입니다.

| 배지 | 의미 |
|---|---|
| 사용중 | 예약이 있고 사람도 감지됨 |
| 예약 없이 사용중 | 사람은 있는데 예약이 없음 (⚠ 확인 필요) |
| 예약됨 (비어있음) | 예약 시간인데 사람이 없음 |
| 비어있음 | 예약도 없고 사람도 없음 |
| 기기 오프라인 | 하트비트가 끊김 (기본 60초) |

---

## 4. 재실 판정과 자동 정리 규칙

- PIR은 전원 인가 후 **30초 안정화 구간**을 무시합니다. (`PIR_WARMUP_MS`)
- 움직임이 감지되면 **2분간 '사용중'을 유지**합니다. (`OCCUPANCY_HOLD_MS`)
  앉아서 회의하는 동안 PIR 출력이 잠깐씩 끊기는 것을 흡수하기 위한 값입니다.
- 예약 시간에 사람이 감지되면 **자동 체크인**됩니다.
- 예약 시작 후 **10분간 아무도 없으면 노쇼**로 처리하고,
  `AUTO_RELEASE=true`(기본)면 그 시간대를 **자동으로 비워** 다른 사람이 쓸 수 있게 합니다.
- 예약 없이 사용 중이면 대시보드에 ⚠ 표시가 뜹니다.

---

## 5. 서버 설정 (환경변수)

| 변수 | 기본값 | 설명 |
|---|---|---|
| `PORT` | `3000` | 서버 포트 |
| `HOST` | `0.0.0.0` | 바인딩 주소 |
| `DATA_DIR` | `server/data` | DB(JSON)와 스냅샷 저장 위치 |
| `DEVICE_TOKEN` | `change-me-token` | 기기 인증 토큰. **운영 시 반드시 변경** |
| `ADMIN_TOKEN` | (없음) | 값을 넣으면 회의실 삭제 등에 `X-Admin-Token` 필요 |
| `OFFLINE_AFTER_SEC` | `60` | 이 시간 동안 하트비트가 없으면 오프라인 |
| `NO_SHOW_MINUTES` | `10` | 노쇼 판정 시간 |
| `AUTO_RELEASE` | `true` | 노쇼 예약 자동 해제 |
| `OPEN_MINUTE` / `CLOSE_MINUTE` | `480` / `1320` | 예약 가능 시간대 (08:00~22:00) |
| `TZ` | 시스템 값 | `Asia/Seoul` 권장 |

### 서비스로 등록 (systemd 예시)

```ini
# /etc/systemd/system/meeting-room.service
[Unit]
Description=Meeting Room Dashboard
After=network.target

[Service]
WorkingDirectory=/opt/meeting-room-system/server
ExecStart=/usr/bin/node index.js
Environment=TZ=Asia/Seoul
Environment=PORT=3000
Environment=DEVICE_TOKEN=바꾸세요
Restart=always

[Install]
WantedBy=multi-user.target
```

하나의 도메인으로 서비스하려면 앞단에 Nginx/Caddy를 두고 `proxy_pass http://127.0.0.1:3000;` 하면 됩니다.

---

## 6. 알아둘 점

- **라이브 영상은 회의실과 같은 네트워크에서만** 열립니다. 기기가 사설망에 있기 때문입니다.
  외부에서는 기기가 30초마다 서버로 올리는 **스냅샷**을 보게 됩니다.
  (원격에서도 라이브가 필요하면 각 기기에 포트포워딩/VPN이 필요합니다.)
- 서버 데이터는 `server/data/db.json` 한 파일에 저장됩니다. 백업은 이 파일만 복사하면 됩니다.
  규모가 커지면 `server/lib/store.js` 만 SQLite 등으로 교체하면 됩니다.
- 기기는 서버가 죽어 있어도 **단독으로 예약을 받아** 두었다가 서버가 살아나면 밀어 올립니다.
- 예약 취소는 **예약자 이름이 일치**해야 가능합니다. (`ADMIN_TOKEN` 을 쓰면 관리자 강제 취소 가능)

자세한 배선은 [docs/HARDWARE.md](docs/HARDWARE.md), API 명세는 [docs/API.md](docs/API.md) 를 보세요.
