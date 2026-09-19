# API 명세

두 종류가 있습니다.

- **브라우저용 JSON API** – 대시보드가 사용
- **기기용 텍스트 API** – ESP32가 사용 (JSON 파서 의존성을 없애려고 파이프 구분 텍스트)

---

## 1. 브라우저용 JSON API

### `GET /api/config`
대시보드가 쓰는 서버 설정값.

```json
{ "openMinute": 480, "closeMinute": 1320, "noShowMinutes": 10,
  "autoRelease": true, "offlineAfterSec": 60, "requiresAdminToken": false }
```

### `GET /api/rooms?date=YYYY-MM-DD`
모든 회의실의 현재 상태 + 해당 날짜 예약.

```json
{
  "serverTime": "2026-09-19T05:00:00.000Z",
  "date": "2026-09-19",
  "nowMin": 840,
  "summary": { "total": 4, "online": 4, "inUse": 2, "free": 2, "alerts": 1 },
  "rooms": [
    {
      "id": "room-1", "name": "3층 대회의실",
      "online": true, "occupied": true, "state": "in_use",
      "ip": "192.168.0.21", "streamPort": 81,
      "streamUrl": "http://192.168.0.21:81/stream",
      "lastMotion": 5, "motionCount": 42, "rssi": -48,
      "hasSnapshot": true, "snapshotAgo": 12,
      "current": { "id": "R...", "start": 840, "end": 900, "title": "주간 회의", "user": "홍길동" },
      "next": null,
      "reservations": [ ... ],
      "unbookedUse": false, "ghostReservation": false
    }
  ]
}
```

`state` 는 `in_use` | `occupied` | `reserved` | `free` | `unreported` 중 하나입니다.
`start`/`end` 는 자정으로부터의 분입니다. (840 = 14:00)

### `GET /api/rooms/:id?date=`
회의실 하나의 상태. 위 `rooms[]` 원소와 같은 형태.

### `GET /api/rooms/:id/snapshot`
기기가 마지막으로 올린 JPEG. 없으면 404.

### `GET /api/reservations?date=&roomId=`
### `POST /api/reservations`
`application/json` 또는 `application/x-www-form-urlencoded`.

| 필드 | 예 | 설명 |
|---|---|---|
| `roomId` | `room-1` | 필수 |
| `date` | `2026-09-19` | 생략 시 오늘 |
| `start`, `end` | `14:00` 또는 `840` | 필수 |
| `title` | `주간 팀 회의` | 생략 시 "회의" |
| `user` | `홍길동` | 생략 시 "익명" |

성공 `201 {"ok":true,"reservation":{...}}` /
겹침·운영시간 위반 등은 `409 {"ok":false,"error":"..."}`.

### `POST /api/reservations/:id/cancel`
`user` 가 예약자 이름과 일치해야 합니다.
`ADMIN_TOKEN` 을 설정한 서버라면 `X-Admin-Token` 헤더로 강제 취소할 수 있습니다.

### `GET /api/events?limit=40`
입실 감지, 노쇼 해제, 예약/취소 등 최근 활동.

### `DELETE /api/rooms/:id`
기기 등록 해제. `ADMIN_TOKEN` 설정 시 `X-Admin-Token` 필요.

---

## 2. 기기용 텍스트 API

모든 요청에 **`X-Device-Token`** 헤더가 필요합니다. (서버의 `DEVICE_TOKEN` 과 동일)
본문은 `application/x-www-form-urlencoded`.

### `POST /api/devices/register`
`roomId, roomName, ip, mac, fw, streamPort, hasCamera` → `OK`

### `POST /api/devices/heartbeat`
`roomId, roomName, occupied(0|1), lastMotion(초, -1=없음), motionCount, rssi, ip, uptime, heap, fw, streamPort`

응답:

```
OK
PULL|1        ← 서버 쪽 예약이 바뀌었으니 목록을 다시 받아가라는 신호
```

### `GET /api/devices/reservations?roomId=&date=`

```
DATE|2026-09-19
NOW|840
RES|RMU7SEA82B2ZP|840|900|주간 팀 회의|홍길동
RES|RMU7SEABLLG7X|960|1020|고객 미팅|이민수
```

필드 구분자는 `|` 이고, 제목/이름에 들어 있는 `|` 와 줄바꿈은 서버가 제거합니다.

### `POST /api/devices/reservations`
`roomId, date, start(분), end(분), title, user, localId`
→ `OK|<서버가 부여한 예약 ID>` / 겹치면 `409 ERR|<사유>`

기기에서 올라온 예약은 운영시간(`OPEN_MINUTE`~`CLOSE_MINUTE`) 제한을 받지 않습니다.
회의실 앞에서 "지금 바로 사용"을 누르는 경우를 막지 않기 위해서입니다.

### `POST /api/devices/cancel`
`roomId, id` → `OK`

### `POST /api/devices/snapshot?roomId=`
본문: JPEG 바이너리 (`Content-Type: image/jpeg`, 최대 1MB) → `OK`

---

## 3. 기기가 직접 제공하는 API

같은 네트워크에서 `http://<기기 IP>/` 로 접근할 수 있습니다.

| 경로 | 설명 |
|---|---|
| `GET /` | 회의실 페이지 (설정 모드에서는 Wi-Fi 설정 포털) |
| `GET /api/status` | 상태 + 오늘 예약 (JSON) |
| `GET /api/reservations?date=` | 예약 목록 (JSON) |
| `POST /api/reservations` | `date, start, end, title, user` |
| `POST /api/quickbook` | `title, user` → 지금부터 30분 |
| `POST /api/reservations/cancel` | `id` |
| `POST /api/wifi/reset` | Wi-Fi 설정 삭제 후 재부팅 |
| `POST /api/restart` | 재부팅 |
| `GET http://<IP>:81/stream` | MJPEG 실시간 영상 |
| `GET http://<IP>:81/capture` | JPEG 스냅샷 1장 |

설정 모드(AP)에서만 쓰이는 경로: `GET /api/info`, `GET /api/scan`,
`POST /api/save`, `GET /api/wifi/status`.
