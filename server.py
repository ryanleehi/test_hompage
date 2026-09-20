#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
회의실 예약 / 재실 감지 서버
============================

ESP32-S3-CAM 클라이언트(esp32-s3-cam-meetingroom.ino)와 연동되는 Flask 서버.

  - 접속하는 클라이언트의 MAC / IP / 회의실명을 clients.csv 에 저장
  - 클라이언트마다 인증코드를 발급하고, 클라이언트 웹페이지에서 입력한 코드를 검증해 토큰 발급
  - 인증된 클라이언트의 재실 상태(heartbeat)와 블러 처리된 스냅샷 수신
  - 회의실 예약: 회의 목적 / 예약자명, 08:00 ~ 20:00 를 30분 단위로 예약 (reservations.csv)
  - 대시보드(/): 회의실 상태 + 좌우로 이동 가능한 시간 막대로 예약
  - 관리자(/admin): 클라이언트 목록 / 인증코드 확인 / 회의실명 확인·수정 / 인증 취소

실행:
    pip install flask
    python server.py                 # http://0.0.0.0:5000
    PORT=8080 ADMIN_PASSWORD=secret python server.py   # 포트 변경, 관리자 페이지 비밀번호
"""

import csv
import json
import os
import secrets
import threading
import time
import uuid
from datetime import datetime, date

from flask import (Flask, Response, abort, jsonify, redirect, render_template_string,
                   request, url_for)


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CLIENTS_CSV = os.path.join(BASE_DIR, "clients.csv")
RESERVATIONS_CSV = os.path.join(BASE_DIR, "reservations.csv")
ROOMS_JSON = os.path.join(BASE_DIR, "rooms.json")            # 층 목록 + 회의실 기본 정보(층/TV/의자/테이블/화이트보드)
ACTIVITY_CSV = os.path.join(BASE_DIR, "activity.csv")        # 예약 생성/수정/삭제 로그
SNAPSHOT_DIR = os.path.join(BASE_DIR, "snapshots")

HOST = os.environ.get("HOST", "0.0.0.0")
PORT = int(os.environ.get("PORT", "5000"))
ADMIN_PASSWORD = os.environ.get("ADMIN_PASSWORD", "")   # 비어 있으면 /admin 인증 없음

OPEN_HOUR, CLOSE_HOUR, SLOT_MIN = 8, 20, 30   # 08:00 ~ 20:00, 30분 단위
OFFLINE_AFTER_SEC = 30                        # heartbeat 가 이 시간 이상 없으면 오프라인
PRESENCE_HOLD_SEC = 30                        # 서버가 스냅샷에서 사람을 찾은 뒤 "재실" 유지 시간 (보드의 PRESENCE_HOLD_MS 와 동일)
MAX_AUTH_FAILS = 5                            # 연속 실패 시 인증코드 재발급

CLIENT_FIELDS = ["mac", "ip", "room", "auth_code", "authorized", "token",
                 "first_seen", "last_seen", "fail_count"]
RES_FIELDS = ["id", "room", "date", "start", "end", "purpose", "reserver", "link", "created_at"]   # link: 줌/구글밋 등 회의 링크


def norm_link(v):
    """회의 링크 정리: 비어 있으면 "", 스킴이 없으면 https:// 를 붙임"""
    v = (v or "").strip()
    if v and not v.lower().startswith(("http://", "https://")):
        v = "https://" + v
    return v

# ---------------------------------------------------------------------------
# 서버측 얼굴 블러 안전장치 (선택):  pip install opencv-python-headless numpy
#   기기에서 얼굴 감지가 꺼져 있거나(PSRAM 없음) 놓친 얼굴이 있어도 서버에서 한 번 더 블러 처리.
#   OpenCV 의 DNN 얼굴 검출기(YuNet)를 사용하고, 모델 파일(약 230KB)은 models/ 에 없으면 시작 시 내려받음.
#   SERVER_BLUR=0 으로 실행하면 끔.
# ---------------------------------------------------------------------------
SERVER_BLUR = os.environ.get("SERVER_BLUR", "1") != "0"     # 스냅샷 모자이크 처리
SERVER_DETECT = os.environ.get("SERVER_DETECT", "1") != "0" # 스냅샷에서 사람/얼굴 검출 -> 재실 판정 (블러와 별개로 동작)
BLUR_MODE = os.environ.get("BLUR_MODE", "person")          # person: 사람 전체 모자이크 / face: 얼굴만
PERSON_CONF = float(os.environ.get("PERSON_CONF", "0.3"))  # YOLOX 사람 검출 신뢰도 임계값 (낮출수록 민감)
MODEL_DIR = os.path.join(BASE_DIR, "models")
YUNET_FILE = os.path.join(MODEL_DIR, "face_detection_yunet_2023mar.onnx")
YUNET_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
HAVE_CV2 = False
_yunet = None
_yunet_lock = threading.Lock()
_cascades = []
_hog = None                 # HOG 보행자 검출기 (OpenCV 4.x 폴백)
_hog_lock = threading.Lock()
_yolox = None               # YOLOX 사람 검출기 (DNN, opencv_zoo) - 얼굴이 안 보이는/돌아선/앉은 사람도 잡음
_yolox_lock = threading.Lock()
YOLOX_FILE = os.path.join(MODEL_DIR, "object_detection_yolox_2022nov.onnx")
YOLOX_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/object_detection_yolox/object_detection_yolox_2022nov.onnx"
YOLOX_SIZE = 640
try:
    import cv2
    import numpy as np
    HAVE_CV2 = True
except Exception:  # noqa: BLE001
    cv2 = None
    np = None


def _download(url, dest, min_size):
    """모델 파일 다운로드 (urllib 실패 시 시스템 curl 로 재시도). 성공 여부 반환"""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print("모델 다운로드 중: %s" % url, flush=True)
    tmp = dest + ".tmp"
    try:
        import urllib.request
        urllib.request.urlretrieve(url, tmp)
    except Exception as e:  # noqa: BLE001
        # python.org 배포판 Python 은 루트 인증서가 없어 SSL 검증에 실패하는 경우가 많음 -> 시스템 curl 로 재시도
        import subprocess
        r = subprocess.run(["curl", "-sSL", "--max-time", "120", "-o", tmp, url])
        if r.returncode != 0:
            print("모델 다운로드 실패 (%s). %s 를 직접 받아 %s 에 넣어주세요" % (e, url, dest), flush=True)
    if os.path.exists(tmp) and os.path.getsize(tmp) > min_size:
        os.replace(tmp, dest)
        return True
    if os.path.exists(tmp):
        os.remove(tmp)
    return False


def init_face_detector():
    """YuNet(DNN) 우선, 없으면 Haar 캐스케이드. 둘 다 없으면 서버측 블러 비활성."""
    global _yunet, _cascades, HAVE_CV2
    if not (HAVE_CV2 and (SERVER_BLUR or SERVER_DETECT)):
        return
    if not os.path.exists(YUNET_FILE):
        _download(YUNET_URL, YUNET_FILE, 100000)
    if os.path.exists(YUNET_FILE) and hasattr(cv2, "FaceDetectorYN"):
        try:
            _yunet = cv2.FaceDetectorYN.create(YUNET_FILE, "", (320, 240), 0.6, 0.3, 500)
            return
        except Exception as e:  # noqa: BLE001
            print("YuNet 초기화 실패: %s" % e, flush=True)
    # 폴백: Haar 캐스케이드 (OpenCV 4.x 패키지에 포함, 5.x 에는 없음)
    try:
        for f in ("haarcascade_frontalface_default.xml", "haarcascade_profileface.xml"):
            path = os.path.join(cv2.data.haarcascades, f)
            if os.path.exists(path):
                c = cv2.CascadeClassifier(path)
                if not c.empty():
                    _cascades.append(c)
    except Exception:  # noqa: BLE001
        pass
    if not _cascades:
        HAVE_CV2 = False


def init_person_detector():
    """사람(전신/상반신) 검출기: YOLOX(DNN, 36MB) 우선, 없으면 HOG(OpenCV 4.x). 얼굴 기준 확장만으로도 동작은 함"""
    global _hog, _yolox
    if not (HAVE_CV2 and (SERVER_DETECT or (SERVER_BLUR and BLUR_MODE == "person"))):
        return
    if not os.path.exists(YOLOX_FILE):
        _download(YOLOX_URL, YOLOX_FILE, 10000000)
    if os.path.exists(YOLOX_FILE):
        try:
            _yolox = cv2.dnn.readNet(YOLOX_FILE)
            return
        except Exception as e:  # noqa: BLE001
            print("YOLOX 초기화 실패: %s" % e, flush=True)
            _yolox = None
    if hasattr(cv2, "HOGDescriptor"):
        try:
            _hog = cv2.HOGDescriptor()
            _hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())
        except Exception as e:  # noqa: BLE001
            print("HOG 보행자 검출기 초기화 실패: %s" % e, flush=True)
            _hog = None


def _detect_faces(img):
    """BGR 이미지에서 얼굴 사각형 [(x, y, w, h), ...] 반환"""
    h, w = img.shape[:2]
    if _yunet is not None:
        with _yunet_lock:                        # FaceDetectorYN 은 스레드 안전하지 않음
            _yunet.setInputSize((w, h))
            _, faces = _yunet.detect(img)
        if faces is None:
            return []
        return [(int(f[0]), int(f[1]), int(f[2]), int(f[3])) for f in faces]
    gray = cv2.equalizeHist(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
    found = []
    for cas in _cascades:
        for flip in (False, True):               # 프로파일 캐스케이드는 한쪽 방향만 잡으므로 좌우 반전도 검사
            g = cv2.flip(gray, 1) if flip else gray
            for (x, y, fw, fh) in cas.detectMultiScale(g, 1.1, 4, minSize=(max(16, w // 20),) * 2):
                found.append((int(w - x - fw) if flip else int(x), int(y), int(fw), int(fh)))
    return found


def _face_to_person(x, y, fw, fh, W, H):
    """얼굴 사각형을 사람(머리~상반신/하반신) 영역으로 확장. 얼굴 폭의 약 4배, 얼굴 높이의 약 7배"""
    cx = x + fw / 2.0
    x1, x2 = cx - 2.0 * fw, cx + 2.0 * fw
    y1, y2 = y - 0.7 * fh, y + fh + 5.5 * fh
    return (max(0, int(x1)), max(0, int(y1)), min(W, int(x2)), min(H, int(y2)))


_yolox_grids = None


def _yolox_decode_tables():
    global _yolox_grids
    if _yolox_grids is None:
        grids, strides = [], []
        for st in (8, 16, 32):
            n = YOLOX_SIZE // st
            yv, xv = np.meshgrid(np.arange(n), np.arange(n), indexing="ij")
            grids.append(np.stack((xv, yv), -1).reshape(-1, 2))
            strides.append(np.full((n * n, 1), st))
        _yolox_grids = (np.concatenate(grids), np.concatenate(strides))
    return _yolox_grids


def _detect_people(img, conf=None):
    conf = PERSON_CONF if conf is None else conf
    """사람 사각형 [(x1, y1, x2, y2), ...] 반환. YOLOX(COCO person) 또는 HOG 폴백"""
    H, W = img.shape[:2]
    if _yolox is not None:
        # letterbox -> 640x640
        r = min(YOLOX_SIZE / H, YOLOX_SIZE / W)
        rs = cv2.resize(img, (max(1, int(W * r)), max(1, int(H * r))))
        pad = np.full((YOLOX_SIZE, YOLOX_SIZE, 3), 114, np.uint8)
        pad[:rs.shape[0], :rs.shape[1]] = rs
        blob = cv2.dnn.blobFromImage(pad, 1.0, (YOLOX_SIZE, YOLOX_SIZE), swapRB=True)
        with _yolox_lock:
            _yolox.setInput(blob)
            out = _yolox.forward()[0]                       # [8400, 85] = cx, cy, w, h, obj, 80 cls
        grids, strides = _yolox_decode_tables()
        xy = (out[:, :2] + grids) * strides
        wh = np.exp(out[:, 2:4]) * strides
        scores = out[:, 4:5] * out[:, 5:]
        cls = scores.argmax(1)
        sc = scores.max(1)
        m = (cls == 0) & (sc > conf)                        # class 0 = person
        boxes = [[int((cx - w / 2) / r), int((cy - h / 2) / r), int(w / r), int(h / r)]
                 for (cx, cy), (w, h) in zip(xy[m], wh[m])]
        if not boxes:
            return []
        idx = cv2.dnn.NMSBoxes(boxes, [float(x) for x in sc[m]], conf, 0.5)
        out_rects = []
        for i in np.array(idx).reshape(-1):
            x, y, w, h = boxes[i]
            out_rects.append((max(0, int(x - w * 0.08)), max(0, int(y - h * 0.05)), min(W, int(x + w * 1.08)), min(H, int(y + h * 1.05))))
        return out_rects
    if _hog is None:
        return []
    scale = 640.0 / W if W < 640 else 1.0          # HOG 검출 창(64x128)보다 사람이 작으면 못 잡으므로 확대
    im = cv2.resize(img, None, fx=scale, fy=scale) if scale != 1.0 else img
    with _hog_lock:
        rects, weights = _hog.detectMultiScale(im, winStride=(8, 8), padding=(8, 8), scale=1.05)
    out_rects = []
    for (x, y, w, h), wt in zip(rects, np.ravel(weights) if len(rects) else []):
        if wt < 0.3:
            continue
        x, y, w, h = x / scale, y / scale, w / scale, h / scale
        out_rects.append((max(0, int(x - w * 0.1)), max(0, int(y - h * 0.05)), min(W, int(x + w * 1.1)), min(H, int(y + h * 1.05))))
    return out_rects


def _person_regions(img):
    """모자이크할 영역 목록과 얼굴 수. BLUR_MODE 에 따라 얼굴만 / 사람 전체"""
    H, W = img.shape[:2]
    faces = _detect_faces(img)
    regions = []
    for (x, y, fw, fh) in faces:
        if BLUR_MODE == "person":
            regions.append(_face_to_person(x, y, fw, fh, W, H))
        else:  # 머리카락/턱까지 덮이도록 조금 확장
            regions.append((max(0, x - fw // 5), max(0, y - fh // 3), min(W, x + fw + fw // 5), min(H, y + fh + fh // 5)))
    if BLUR_MODE == "person" or SERVER_DETECT:
        for r in _detect_people(img):
            # 이미 얼굴 기준 영역에 대부분 포함되면 생략
            rx1, ry1, rx2, ry2 = r
            covered = False
            for (x1, y1, x2, y2) in regions:
                ix = max(0, min(rx2, x2) - max(rx1, x1))
                iy = max(0, min(ry2, y2) - max(ry1, y1))
                if ix * iy > 0.7 * (rx2 - rx1) * (ry2 - ry1):
                    covered = True
                    break
            if not covered:
                regions.append(r)
    return regions, len(faces)

app = Flask(__name__)
app.config["JSON_AS_ASCII"] = False
app.json.ensure_ascii = False

lock = threading.RLock()
clients = {}       # mac -> dict(CLIENT_FIELDS)
live = {}          # mac -> {"occupied", "faces", "rssi", "fps", "last_heartbeat", "snapshot", "snapshot_time"}


# ---------------------------------------------------------------------------
# 유틸
# ---------------------------------------------------------------------------
def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def norm_mac(mac):
    return (mac or "").strip().upper().replace("-", ":")


def gen_code():
    return "%06d" % secrets.randbelow(1000000)


def to_min(hhmm):
    h, m = hhmm.split(":")
    return int(h) * 60 + int(m)


def to_hhmm(minutes):
    return "%02d:%02d" % (minutes // 60, minutes % 60)


def slot_times():
    """08:00, 08:30, ... 20:00 (경계 포함)"""
    return [to_hhmm(m) for m in range(OPEN_HOUR * 60, CLOSE_HOUR * 60 + 1, SLOT_MIN)]


def detector_status():
    """대시보드/로그용 검출기 상태 문구"""
    if not HAVE_CV2:
        return "사용 안 함 - opencv 미설치 (pip install opencv-python-headless numpy)"
    parts = []
    parts.append("얼굴 " + ("YuNet" if _yunet is not None else ("Haar" if _cascades else "없음")))
    parts.append("사람 " + ("YOLOX" if _yolox is not None else ("HOG" if _hog is not None else "없음(models/ 확인)")))
    parts.append("블러 " + (("사람전체" if BLUR_MODE == "person" else "얼굴만") if SERVER_BLUR else "끔"))
    parts.append("재실판정 " + ("켬" if SERVER_DETECT else "끔"))
    return " · ".join(parts)


def blur_faces(jpeg_bytes):
    """JPEG 바이트에서 사람(BLUR_MODE=person) 또는 얼굴(face)을 찾아 모자이크 처리. (처리된 JPEG, 찾은 수) 반환.
    OpenCV/모델이 없거나 디코딩 실패 시 원본 그대로 반환."""
    if not (HAVE_CV2 and (SERVER_BLUR or SERVER_DETECT)):
        return jpeg_bytes, 0
    try:
        img = cv2.imdecode(np.frombuffer(jpeg_bytes, np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            return jpeg_bytes, 0
        regions, nfaces = _person_regions(img)
        if not regions or not SERVER_BLUR:          # 검출만 하고 블러는 안 하는 경우
            return jpeg_bytes, max(nfaces, len(regions))
        for (x1, y1, x2, y2) in regions:
            if x2 <= x1 or y2 <= y1:
                continue
            block = max(8, min(x2 - x1, y2 - y1) // 10)   # 영역 크기에 비례한 모자이크 블록
            roi = img[y1:y2, x1:x2]
            small = cv2.resize(roi, (max(1, roi.shape[1] // block), max(1, roi.shape[0] // block)), interpolation=cv2.INTER_LINEAR)
            img[y1:y2, x1:x2] = cv2.resize(small, (roi.shape[1], roi.shape[0]), interpolation=cv2.INTER_NEAREST)
        ok, enc = cv2.imencode(".jpg", img, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
        return (enc.tobytes() if ok else jpeg_bytes), max(nfaces, len(regions))
    except Exception:  # noqa: BLE001
        return jpeg_bytes, 0


def valid_date(s):
    try:
        datetime.strptime(s, "%Y-%m-%d")
        return True
    except (ValueError, TypeError):
        return False


# ---------------------------------------------------------------------------
# CSV 저장/로드
# ---------------------------------------------------------------------------
def load_clients():
    global clients
    clients = {}
    if not os.path.exists(CLIENTS_CSV):
        return
    with open(CLIENTS_CSV, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            mac = norm_mac(row.get("mac"))
            if not mac:
                continue
            for k in CLIENT_FIELDS:
                row.setdefault(k, "")
            row["mac"] = mac
            clients[mac] = row


def save_clients():
    tmp = CLIENTS_CSV + ".tmp"
    with open(tmp, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CLIENT_FIELDS, extrasaction="ignore")
        w.writeheader()
        for row in clients.values():
            w.writerow(row)
    os.replace(tmp, CLIENTS_CSV)


LOG_FIELDS = ["ts", "action", "room", "date", "start", "end", "reserver", "purpose", "detail", "ip", "id"]


def log_activity(action, r, detail="", rid=None):
    """예약 활동 로그 한 줄 추가 (action: 예약 / 수정 / 삭제)"""
    row = {"ts": now_str(), "action": action, "room": r.get("room", ""), "date": r.get("date", ""),
           "start": r.get("start", ""), "end": r.get("end", ""), "reserver": r.get("reserver", ""),
           "purpose": r.get("purpose", ""), "detail": detail, "ip": request.remote_addr or "", "id": rid or r.get("id", "")}
    new = not os.path.exists(ACTIVITY_CSV)
    try:
        with open(ACTIVITY_CSV, "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=LOG_FIELDS, extrasaction="ignore")
            if new:
                w.writeheader()
            w.writerow(row)
    except OSError as e:
        print("activity.csv 기록 실패: %s" % e, flush=True)


def load_activity(limit=500):
    if not os.path.exists(ACTIVITY_CSV):
        return []
    with open(ACTIVITY_CSV, newline="", encoding="utf-8") as f:
        rows = [dict(r) for r in csv.DictReader(f)]
    rows.reverse()                       # 최신순
    return rows[:limit]


def load_reservations():
    if not os.path.exists(RESERVATIONS_CSV):
        return []
    with open(RESERVATIONS_CSV, newline="", encoding="utf-8") as f:
        rows = [dict(r) for r in csv.DictReader(f)]
    for r in rows:                      # 예전 파일(link 컬럼 없음) 호환
        r["link"] = r.get("link") or ""
    return rows


def save_reservations(rows):
    tmp = RESERVATIONS_CSV + ".tmp"
    with open(tmp, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=RES_FIELDS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    os.replace(tmp, RESERVATIONS_CSV)


# ---------------------------------------------------------------------------
# 클라이언트(기기) 상태 계산
# ---------------------------------------------------------------------------
def client_view(mac):
    c = clients[mac]
    lv = live.get(mac, {})
    last_hb = lv.get("last_heartbeat", 0)
    online = c.get("authorized") == "1" and (time.time() - last_hb) < OFFLINE_AFTER_SEC
    # 재실 = 보드가 보고한 occupied  OR  서버가 최근 스냅샷에서 사람/얼굴을 찾음 (보드 얼굴 감지가 꺼져 있어도 동작)
    server_seen = (time.time() - lv.get("server_person_time", 0)) < PRESENCE_HOLD_SEC
    occupied = online and (bool(lv.get("occupied")) or server_seen)
    faces = max(int(lv.get("faces", 0)), int(lv.get("server_faces", 0)) if server_seen else 0) if online else 0
    return {
        "mac": mac,
        "ip": c.get("ip", ""),
        "room": c.get("room", ""),
        "authorized": c.get("authorized") == "1",
        "online": online,
        "occupied": occupied,
        "faces": faces,
        "occupied_by_server": online and server_seen and not bool(lv.get("occupied")),
        "rssi": lv.get("rssi"),
        "fps": lv.get("fps"),
        "last_seen": c.get("last_seen", ""),
        "last_heartbeat_ago": int(time.time() - last_hb) if last_hb else None,
        "detect": lv.get("detect"),                 # None: 알 수 없음, False: 기기 얼굴 감지 꺼짐
        "server_faces": int(lv.get("server_faces", 0)),
        "has_snapshot": bool(lv.get("snapshot")),
        "snapshot_age": int(time.time() - lv["snapshot_time"]) if lv.get("snapshot_time") else None,
        "stream_url": "http://%s:81/stream" % c.get("ip", "") if c.get("ip") else "",
        "device_url": "http://%s/" % c.get("ip", "") if c.get("ip") else "",
    }


def current_reservation(room, rows=None, when=None):
    when = when or datetime.now()
    rows = rows if rows is not None else load_reservations()
    d = when.strftime("%Y-%m-%d")
    nowm = when.hour * 60 + when.minute
    for r in rows:
        if r["room"] == room and r["date"] == d and to_min(r["start"]) <= nowm < to_min(r["end"]):
            return r
    return None


def check_device_token(mac, token):
    c = clients.get(mac)
    return bool(c and c.get("authorized") == "1" and c.get("token") and c["token"] == token)


# ---------------------------------------------------------------------------
# 기기 API (ESP32 -> 서버)
# ---------------------------------------------------------------------------
@app.post("/api/register")
def api_register():
    d = request.get_json(silent=True) or {}
    mac = norm_mac(d.get("mac"))
    if not mac:
        return jsonify(ok=False, error="mac required"), 400
    ip = (d.get("ip") or request.remote_addr or "").strip()
    room = (d.get("room") or "").strip()
    token = d.get("token") or ""
    with lock:
        c = clients.get(mac)
        is_new = c is None
        if is_new:
            c = {k: "" for k in CLIENT_FIELDS}
            c.update(mac=mac, auth_code=gen_code(), authorized="0", first_seen=now_str(), fail_count="0")
            clients[mac] = c
        c["ip"] = ip
        if room:
            c["room"] = room
        c["last_seen"] = now_str()
        save_clients()
        authorized = check_device_token(mac, token)
        code = c["auth_code"]
    if is_new:
        print("\n[NEW CLIENT] MAC=%s IP=%s 회의실=%s  ->  인증코드: %s\n" % (mac, ip, room, code), flush=True)
    elif not authorized:
        print("[CLIENT] 미인증 기기 접속 MAC=%s IP=%s 회의실=%s  인증코드: %s" % (mac, ip, room, code), flush=True)
    msg = "인증됨" if authorized else "관리자 페이지(/admin)에서 인증코드를 확인 후 입력하세요"
    return jsonify(ok=True, authorized=authorized, message=msg)


@app.post("/api/auth")
def api_auth():
    d = request.get_json(silent=True) or {}
    mac = norm_mac(d.get("mac"))
    code = (d.get("code") or "").strip()
    with lock:
        c = clients.get(mac)
        if not c:
            return jsonify(ok=False, error="등록되지 않은 기기입니다. 잠시 후 다시 시도하세요"), 404
        c["ip"] = (d.get("ip") or request.remote_addr or c.get("ip", "")).strip()
        if d.get("room"):
            c["room"] = d["room"].strip()
        c["last_seen"] = now_str()
        if not code or code != c["auth_code"]:
            fails = int(c.get("fail_count") or 0) + 1
            c["fail_count"] = str(fails)
            if fails >= MAX_AUTH_FAILS:
                c["auth_code"] = gen_code()
                c["fail_count"] = "0"
                print("[AUTH] MAC=%s 연속 실패로 인증코드 재발급: %s" % (mac, c["auth_code"]), flush=True)
                save_clients()
                return jsonify(ok=False, error="연속 실패로 인증코드가 재발급되었습니다. 관리자 페이지를 확인하세요"), 401
            save_clients()
            return jsonify(ok=False, error="인증코드가 올바르지 않습니다 (%d/%d)" % (fails, MAX_AUTH_FAILS)), 401
        token = secrets.token_hex(16)
        c.update(authorized="1", token=token, fail_count="0")
        save_clients()
    print("[AUTH] MAC=%s 인증 성공 (회의실: %s)" % (mac, c.get("room")), flush=True)
    return jsonify(ok=True, token=token, room=c.get("room", ""))


@app.post("/api/heartbeat")
def api_heartbeat():
    d = request.get_json(silent=True) or {}
    mac = norm_mac(d.get("mac"))
    with lock:
        if not check_device_token(mac, d.get("token") or ""):
            return jsonify(ok=False, error="unauthorized"), 401
        c = clients[mac]
        c["ip"] = (d.get("ip") or request.remote_addr or c.get("ip", "")).strip()
        if d.get("room"):
            c["room"] = d["room"].strip()
        c["last_seen"] = now_str()
        lv = live.setdefault(mac, {})
        lv.update(occupied=bool(d.get("occupied")), faces=int(d.get("faces") or 0),
                  rssi=d.get("rssi"), fps=d.get("fps"), last_heartbeat=time.time())
        if "detect" in d:
            lv["detect"] = bool(d.get("detect"))   # 기기 얼굴 감지(블러) 사용 가능 여부
        # last_seen 만 바뀌므로 매번 저장하지 않고 1분에 한 번만 기록
        if time.time() - lv.get("last_saved", 0) > 60:
            lv["last_saved"] = time.time()
            save_clients()
    return jsonify(ok=True)


@app.post("/api/snapshot")
def api_snapshot():
    mac = norm_mac(request.headers.get("X-Device-Mac"))
    token = request.headers.get("X-Device-Token", "")
    data = request.get_data()
    with lock:
        if not check_device_token(mac, token):
            return jsonify(ok=False, error="unauthorized"), 401
    if not data or len(data) < 100:
        return jsonify(ok=False, error="empty"), 400
    # 기기에서 블러가 됐더라도 서버에서 한 번 더 (기기 얼굴 감지가 꺼진 경우의 안전장치). lock 밖에서 수행
    data, server_faces = blur_faces(data)
    with lock:
        lv = live.setdefault(mac, {})
        lv["snapshot"] = data
        lv["server_faces"] = server_faces
        if server_faces > 0:
            lv["server_person_time"] = time.time()   # 서버측 검출도 재실 판정에 반영
        if request.headers.get("X-Device-Detect") is not None:
            lv["detect"] = request.headers.get("X-Device-Detect") == "1"
        lv["snapshot_time"] = time.time()
        # 스냅샷은 heartbeat 를 겸함
        lv["last_heartbeat"] = time.time()
        if request.headers.get("X-Device-Occupied") is not None:
            lv["occupied"] = request.headers.get("X-Device-Occupied") == "1"
        if request.headers.get("X-Device-Faces") is not None:
            try:
                lv["faces"] = int(request.headers.get("X-Device-Faces"))
            except ValueError:
                pass
    try:
        os.makedirs(SNAPSHOT_DIR, exist_ok=True)
        with open(os.path.join(SNAPSHOT_DIR, mac.replace(":", "") + ".jpg"), "wb") as f:
            f.write(data)
    except OSError:
        pass
    return jsonify(ok=True)


# ---------------------------------------------------------------------------
# 대시보드 / 예약 API (브라우저 -> 서버)
# ---------------------------------------------------------------------------
@app.get("/api/rooms")
def api_rooms():
    with lock:
        rows = load_reservations()
        rcfg = load_rooms_config()
        out = []
        for mac in clients:
            v = client_view(mac)
            if not v["authorized"]:
                continue
            cur = current_reservation(v["room"], rows) if v["room"] else None
            v["current_reservation"] = cur
            v["info"] = dict(DEFAULT_ROOM_INFO, **rcfg["rooms"].get(v["room"], {}))
            out.append(v)
    out.sort(key=lambda x: (x["room"] or "~", x["mac"]))
    return jsonify(rooms=out, server_time=now_str())


def all_room_names(cfg=None):
    """카메라 기기 / 예약 내역 / 설정에 등장하는 모든 회의실명"""
    names = {c.get("room") for c in clients.values() if c.get("authorized") == "1" and c.get("room")}
    names |= {r["room"] for r in load_reservations()}
    names |= set((cfg or load_rooms_config())["rooms"].keys())
    names.discard("")
    return sorted(names)


@app.get("/api/room-names")
def api_room_names():
    with lock:
        return jsonify(rooms=all_room_names())


# ---------------------------------------------------------------------------
# 층 / 회의실 기본 정보 설정 (rooms.json)
# ---------------------------------------------------------------------------
DEFAULT_ROOM_INFO = {"floor": "", "tv": False, "chairs": 0, "tables": 0, "whiteboard": False, "vc": False, "note": ""}   # vc: 화상회의 장비


def load_rooms_config():
    cfg = {"floors": [], "rooms": {}, "favorite": ""}   # favorite: 자주 이용하는 회의실 (접속 시 그 층 탭으로 이동)
    if os.path.exists(ROOMS_JSON):
        try:
            with open(ROOMS_JSON, encoding="utf-8") as f:
                data = json.load(f)
            cfg["floors"] = [str(x) for x in data.get("floors", []) if str(x).strip()]
            cfg["favorite"] = str(data.get("favorite") or "").strip()
            for name, info in (data.get("rooms") or {}).items():
                merged = dict(DEFAULT_ROOM_INFO)
                merged.update({k: info.get(k, v) for k, v in DEFAULT_ROOM_INFO.items()})
                cfg["rooms"][name] = merged
        except (OSError, ValueError) as e:
            print("rooms.json 읽기 실패: %s" % e, flush=True)
    return cfg


def save_rooms_config(cfg):
    tmp = ROOMS_JSON + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    os.replace(tmp, ROOMS_JSON)


@app.get("/api/config")
def api_config_get():
    with lock:
        cfg = load_rooms_config()
        names = all_room_names(cfg)
        rooms = {n: dict(DEFAULT_ROOM_INFO, **cfg["rooms"].get(n, {})) for n in names}
    return jsonify(floors=cfg["floors"], rooms=rooms, favorite=cfg.get("favorite", ""))


@app.post("/api/config")
def api_config_set():
    d = request.get_json(silent=True) or {}
    floors, seen = [], set()
    for f in d.get("floors") or []:
        f = str(f).strip()
        if f and f not in seen:
            floors.append(f)
            seen.add(f)
    rooms = {}
    for name, info in (d.get("rooms") or {}).items():
        name = str(name).strip()
        if not name:
            continue
        info = info or {}
        try:
            chairs = max(0, int(info.get("chairs") or 0))
            tables = max(0, int(info.get("tables") or 0))
        except (TypeError, ValueError):
            return jsonify(ok=False, error="의자/테이블 수는 숫자여야 합니다 (%s)" % name), 400
        floor = str(info.get("floor") or "").strip()
        if floor and floor not in seen:
            return jsonify(ok=False, error="'%s' 의 층 '%s' 이(가) 층 목록에 없습니다" % (name, floor)), 400
        rooms[name] = {"floor": floor, "tv": bool(info.get("tv")), "chairs": chairs, "tables": tables,
                       "whiteboard": bool(info.get("whiteboard")), "vc": bool(info.get("vc")),
                       "note": str(info.get("note") or "").strip()}
    favorite = str(d.get("favorite") or "").strip()
    with lock:
        save_rooms_config({"floors": floors, "rooms": rooms, "favorite": favorite})
    print("[CONFIG] 층 %s, 회의실 %d개 저장" % (floors, len(rooms)), flush=True)
    return jsonify(ok=True)


@app.get("/snapshot/<mac>.jpg")
def snapshot_image(mac):
    mac = norm_mac(mac)
    with lock:
        data = live.get(mac, {}).get("snapshot")
    if not data:
        abort(404)
    return Response(data, mimetype="image/jpeg", headers={"Cache-Control": "no-store"})


@app.get("/api/reservations")
def api_reservations_list():
    room = request.args.get("room", "")
    d = request.args.get("date", "")
    with lock:
        rows = load_reservations()
    if room:
        rows = [r for r in rows if r["room"] == room]
    if d:
        rows = [r for r in rows if r["date"] == d]
    rows.sort(key=lambda r: (r["date"], r["room"], r["start"]))
    return jsonify(reservations=rows, slots=slot_times())


@app.post("/api/reservations")
def api_reservations_create():
    d = request.get_json(silent=True) or {}
    room = (d.get("room") or "").strip()
    day = (d.get("date") or "").strip()
    start = (d.get("start") or "").strip()
    end = (d.get("end") or "").strip()
    purpose = (d.get("purpose") or "").strip()
    link = norm_link(d.get("link"))
    reserver = (d.get("reserver") or "").strip()

    slots = slot_times()
    if not room:
        return jsonify(ok=False, error="회의실을 선택하세요"), 400
    if not valid_date(day):
        return jsonify(ok=False, error="날짜 형식이 올바르지 않습니다"), 400
    if start not in slots or end not in slots:
        return jsonify(ok=False, error="시간은 %02d:00 ~ %02d:00 사이 30분 단위여야 합니다" % (OPEN_HOUR, CLOSE_HOUR)), 400
    if to_min(start) >= to_min(end):
        return jsonify(ok=False, error="종료 시간은 시작 시간보다 늦어야 합니다"), 400
    if not reserver:
        return jsonify(ok=False, error="예약자명을 입력하세요"), 400
    if not purpose:
        return jsonify(ok=False, error="회의 목적을 입력하세요"), 400

    with lock:
        rows = load_reservations()
        for r in rows:
            if r["room"] == room and r["date"] == day and to_min(start) < to_min(r["end"]) and to_min(r["start"]) < to_min(end):
                return jsonify(ok=False, error="이미 예약된 시간과 겹칩니다 (%s~%s %s)" % (r["start"], r["end"], r["reserver"])), 409
        new = {"id": uuid.uuid4().hex[:10], "room": room, "date": day, "start": start, "end": end,
               "purpose": purpose, "reserver": reserver, "link": link, "created_at": now_str()}
        rows.append(new)
        save_reservations(rows)
        log_activity("예약", new)
    print("[RESERVE] %s %s %s~%s %s (%s)" % (room, day, start, end, reserver, purpose), flush=True)
    return jsonify(ok=True, reservation=new)


@app.route("/api/reservations/<rid>", methods=["PATCH", "PUT"])
def api_reservations_update(rid):
    """예약 시간(또는 목적/예약자) 변경. 시간 막대에서 블록을 끌어 옮길 때 사용"""
    d = request.get_json(silent=True) or {}
    slots = slot_times()
    with lock:
        rows = load_reservations()
        cur = next((r for r in rows if r["id"] == rid), None)
        if not cur:
            return jsonify(ok=False, error="예약을 찾을 수 없습니다"), 404
        start = (d.get("start") or cur["start"]).strip()
        end = (d.get("end") or cur["end"]).strip()
        day = (d.get("date") or cur["date"]).strip()
        purpose = (d.get("purpose") or cur["purpose"]).strip()
        link = norm_link(d["link"]) if "link" in d else cur.get("link", "")
        reserver = (d.get("reserver") or cur["reserver"]).strip()
        if not valid_date(day):
            return jsonify(ok=False, error="날짜 형식이 올바르지 않습니다"), 400
        if start not in slots or end not in slots:
            return jsonify(ok=False, error="시간은 %02d:00 ~ %02d:00 사이 30분 단위여야 합니다" % (OPEN_HOUR, CLOSE_HOUR)), 400
        if to_min(start) >= to_min(end):
            return jsonify(ok=False, error="종료 시간은 시작 시간보다 늦어야 합니다"), 400
        for r in rows:
            if r["id"] != rid and r["room"] == cur["room"] and r["date"] == day and to_min(start) < to_min(r["end"]) and to_min(r["start"]) < to_min(end):
                return jsonify(ok=False, error="이미 예약된 시간과 겹칩니다 (%s~%s %s)" % (r["start"], r["end"], r["reserver"])), 409
        old = "%s %s~%s" % (cur["date"], cur["start"], cur["end"])
        changes = []
        if (cur["date"], cur["start"], cur["end"]) != (day, start, end):
            changes.append("시간 %s → %s %s~%s" % (old, day, start, end))
        if cur["reserver"] != reserver:
            changes.append("예약자 %s → %s" % (cur["reserver"], reserver))
        if cur["purpose"] != purpose:
            changes.append("목적 %s → %s" % (cur["purpose"], purpose))
        if cur.get("link", "") != link:
            changes.append("링크 " + ("추가" if not cur.get("link") else ("삭제" if not link else "변경")))
        cur.update(date=day, start=start, end=end, purpose=purpose, reserver=reserver, link=link)
        save_reservations(rows)
        log_activity("수정", cur, ", ".join(changes) or "변경 없음")
    print("[MOVE] %s %s -> %s %s~%s (%s)" % (cur["room"], old, day, start, end, reserver), flush=True)
    return jsonify(ok=True, reservation=cur)


@app.delete("/api/reservations/<rid>")
def api_reservations_delete(rid):
    with lock:
        rows = load_reservations()
        keep = [r for r in rows if r["id"] != rid]
        if len(keep) == len(rows):
            return jsonify(ok=False, error="예약을 찾을 수 없습니다"), 404
        gone = next(r for r in rows if r["id"] == rid)
        save_reservations(keep)
        log_activity("삭제", gone)
    return jsonify(ok=True)


# ---------------------------------------------------------------------------
# 대시보드 통계 / 활동 로그
# ---------------------------------------------------------------------------
@app.get("/api/stats")
def api_stats():
    """period: all | 30 | 90 | year  (예약 날짜 기준)"""
    period = request.args.get("period", "all")
    today = date.today()
    with lock:
        rows = load_reservations()
        occupied = sum(1 for mac in clients if client_view(mac)["occupied"])
        cams = sum(1 for mac in clients if clients[mac].get("authorized") == "1")
    def in_period(r):
        try:
            d = datetime.strptime(r["date"], "%Y-%m-%d").date()
        except ValueError:
            return False
        if period == "30":
            return (today - d).days <= 30 and d <= today
        if period == "90":
            return (today - d).days <= 90 and d <= today
        if period == "year":
            return d.year == today.year
        return True
    sel = [r for r in rows if in_period(r)]
    durs = [to_min(r["end"]) - to_min(r["start"]) for r in sel]
    total_min = sum(durs)
    by_dow = [0] * 7                       # 월..일
    by_hour = [0] * (CLOSE_HOUR - OPEN_HOUR)   # 해당 시간대에 진행된 회의 수
    by_room = {}
    by_month = {}
    for r in sel:
        d = datetime.strptime(r["date"], "%Y-%m-%d").date()
        by_dow[d.weekday()] += 1
        by_room[r["room"]] = by_room.get(r["room"], 0) + 1
        by_month[d.strftime("%Y-%m")] = by_month.get(d.strftime("%Y-%m"), 0) + 1
        for h in range(OPEN_HOUR, CLOSE_HOUR):
            if to_min(r["start"]) < (h + 1) * 60 and h * 60 < to_min(r["end"]):
                by_hour[h - OPEN_HOUR] += 1
    # 월별: 최근 12개월 (없는 달은 0)
    months = []
    y, m = today.year, today.month
    for _ in range(12):
        months.append("%04d-%02d" % (y, m))
        m -= 1
        if m == 0:
            y, m = y - 1, 12
    months.reverse()
    this_month = today.strftime("%Y-%m")
    return jsonify(
        period=period, total=len(sel), total_minutes=total_min,
        avg_minutes=round(total_min / len(sel)) if sel else 0,
        today=sum(1 for r in rows if r["date"] == today.isoformat()),
        this_month=sum(1 for r in rows if r["date"].startswith(this_month)),
        occupied_rooms=occupied, camera_rooms=cams,
        by_dow=by_dow, by_hour=by_hour, hours=list(range(OPEN_HOUR, CLOSE_HOUR)),
        by_month=[{"month": k, "count": by_month.get(k, 0)} for k in months],
        by_room=sorted([{"room": k, "count": v} for k, v in by_room.items()], key=lambda x: -x["count"])[:8],
        reservers=sorted([{"name": k, "count": v} for k, v in
                          {r["reserver"]: sum(1 for x in sel if x["reserver"] == r["reserver"]) for r in sel}.items()],
                         key=lambda x: -x["count"])[:5],
    )


@app.get("/api/log")
def api_log():
    try:
        limit = max(1, min(2000, int(request.args.get("limit", "300"))))
    except ValueError:
        limit = 300
    with lock:
        rows = load_activity(limit)
    return jsonify(log=rows)


# ---------------------------------------------------------------------------
# 관리자
# ---------------------------------------------------------------------------
def admin_required():
    if not ADMIN_PASSWORD:
        return None
    a = request.authorization
    if a and a.password == ADMIN_PASSWORD:
        return None
    return Response("인증 필요", 401, {"WWW-Authenticate": 'Basic realm="meetingroom admin"'})


@app.get("/admin")
def admin_page():
    r = admin_required()
    if r:
        return r
    with lock:
        rows = []
        for mac in sorted(clients):
            v = client_view(mac)
            v["auth_code"] = clients[mac]["auth_code"]
            v["first_seen"] = clients[mac].get("first_seen", "")
            rows.append(v)
    return render_template_string(ADMIN_HTML, rows=rows, csv_path=CLIENTS_CSV, server_time=now_str())


@app.post("/admin/action")
def admin_action():
    r = admin_required()
    if r:
        return r
    mac = norm_mac(request.form.get("mac"))
    action = request.form.get("action")
    with lock:
        c = clients.get(mac)
        if not c:
            abort(404)
        if action == "revoke":
            c.update(authorized="0", token="", auth_code=gen_code(), fail_count="0")
            live.pop(mac, None)
        elif action == "regen":
            c.update(auth_code=gen_code(), fail_count="0")
        elif action == "room":
            c["room"] = (request.form.get("room") or "").strip()
        elif action == "delete":
            del clients[mac]
            live.pop(mac, None)
        save_clients()
    return redirect(url_for("admin_page"))


# ---------------------------------------------------------------------------
# 대시보드 페이지
# ---------------------------------------------------------------------------
@app.get("/")
def index():
    html = render_template_string(INDEX_HTML, open_hour=OPEN_HOUR, close_hour=CLOSE_HOUR,
                                  slot_min=SLOT_MIN, today=date.today().isoformat(),
                                  server_blur=HAVE_CV2 and SERVER_BLUR, detector=detector_status(),
                                  detector_ok=HAVE_CV2 and (_yolox is not None or _hog is not None or _yunet is not None or bool(_cascades)))
    # 브라우저가 예전 JS 를 캐시해 쓰지 않도록
    return Response(html, mimetype="text/html", headers={"Cache-Control": "no-store, no-cache, must-revalidate, max-age=0"})


BASE_CSS = """
:root{--bg:#f3f5f7;--card:#fff;--fg:#1c2430;--muted:#66717f;--line:#dde2e8;--acc:#2563eb;--acc2:#dbeafe;--ok:#16a34a;--warn:#d97706;--bad:#dc2626}
*{box-sizing:border-box}body{margin:0;font-family:-apple-system,"Apple SD Gothic Neo","Malgun Gothic",sans-serif;background:var(--bg);color:var(--fg);font-size:14px}
header{background:#111827;color:#fff;padding:12px 20px;display:flex;align-items:center;gap:16px}header h1{font-size:18px;margin:0;font-weight:600}
header a{color:#cbd5e1;text-decoration:none;font-size:13px}header .sp{flex:1}
main{max-width:1200px;margin:0 auto;padding:18px;display:grid;gap:16px}
/* grid 아이템은 기본 min-width:auto 라서 안쪽의 넓은 시간 막대가 카드를 화면보다 넓게 늘림 → 0 으로 고정 */
main>*{min-width:0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;max-width:100%}.card h2{font-size:16px;margin:0 0 12px}
input,select,button,textarea{font:inherit}input[type=text],input[type=date],input[type=password],select{padding:8px 10px;border:1px solid var(--line);border-radius:6px;background:#fff}
button{padding:8px 14px;border:none;border-radius:6px;background:var(--acc);color:#fff;cursor:pointer;white-space:nowrap}button.sec{background:#e5e7eb;color:var(--fg)}button.bad{background:var(--bad)}button.sm{padding:4px 9px;font-size:12px}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}label{font-size:12px;color:var(--muted);display:block;margin-bottom:3px}
.badge{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;font-weight:600;color:#fff;background:#9ca3af}.badge.ok{background:var(--ok)}.badge.warn{background:var(--warn)}.badge.bad{background:var(--bad)}.badge.acc{background:var(--acc)}
table{border-collapse:collapse;width:100%}th,td{padding:8px 6px;border-bottom:1px solid var(--line);text-align:left;vertical-align:middle}th{font-size:12px;color:var(--muted);font-weight:600}
.muted{color:var(--muted);font-size:12px}code{background:#f1f5f9;padding:1px 5px;border-radius:4px}
"""

INDEX_HTML = r"""<!DOCTYPE html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>회의실 예약 시스템</title>
<style>
""" + BASE_CSS + r"""
.rooms{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px}
/* 탭 */
.tabs{display:flex;gap:4px;max-width:1200px;margin:14px auto -16px;padding:0 18px;flex-wrap:wrap;align-items:flex-end}
.tab{padding:9px 18px;border:1px solid var(--line);border-bottom:none;border-radius:9px 9px 0 0;background:#e9edf2;color:var(--muted);cursor:pointer;font-weight:600;font-size:14px;line-height:20px;height:39px;box-sizing:border-box}
.tab:hover{background:#f3f5f8}.tab.active{background:#fff;color:var(--fg);box-shadow:0 -2px 0 var(--acc) inset}.tab.cfg{margin-left:6px}
.tabclock{margin-left:auto;align-self:center;font-size:15px;font-weight:600;color:var(--fg);padding:0 10px 6px;white-space:nowrap;display:flex;align-items:center}
.tabclock .dow{color:var(--acc)}.tabclock .sun{color:var(--bad)}
.tabclock .sep{color:#c7ccd3;font-weight:400;margin:0 10px}
.tabclock .time{display:inline-block;width:5em;text-align:left;font-variant-numeric:tabular-nums;font-family:ui-monospace,Menlo,Consolas,monospace}   /* 고정폭: 초가 바뀌어도 날짜 위치가 움직이지 않음 */
/* 층 탭 화면: 왼쪽 카메라 | 가운데 회의실 정보 | 오른쪽 예약 목록 */
.cols3{display:grid;grid-template-columns:minmax(270px,1fr) minmax(200px,.68fr) minmax(340px,1.32fr);grid-template-rows:auto 1fr;gap:16px}   /* 회의실 정보 열을 80% 로 줄이고 그만큼 예약 목록 열을 넓힘 */
.cols3>.cams{grid-row:1/3;grid-column:1}          /* 회의실 현황: 왼쪽 열 전체 높이 */
.cols3>.resv{grid-column:2/4}                      /* 예약 섹션: 회의실 정보 + 예약 목록 아래, 그 두 카드 폭만큼 */
.cols3>.card{min-width:0;display:flex;flex-direction:column}.cols3 .rooms{grid-template-columns:1fr;align-content:start}
/* 왼쪽(회의실 현황)/오른쪽(예약 목록) 내용은 절대 배치 -> 행 높이에 영향을 주지 않고, 행 높이는 가운데 '회의실 정보' 카드의 내용 높이로 결정됨 */
.cols3 .fill{flex:1 1 auto;position:relative;min-height:160px}
tr.now td{color:var(--ok);font-weight:600}tr.now .badge{margin-left:6px;vertical-align:1px}
#resList td:nth-child(2){white-space:nowrap}#resList td:nth-child(4){text-align:center;width:30px}#resList td:nth-child(5){white-space:nowrap;width:56px}.cols3 .fill>.scroll{position:absolute;inset:0;overflow-y:auto;overflow-x:hidden;padding-right:4px}
.cols3 .fill>.scroll::-webkit-scrollbar{width:8px}.cols3 .fill>.scroll::-webkit-scrollbar-thumb{background:#cbd5e1;border-radius:4px}
@media (max-width:1000px){.cols3{grid-template-columns:1fr;grid-template-rows:none}.cols3>.cams{grid-row:auto;grid-column:auto}.cols3>.resv{grid-column:auto}.cols3 .fill{min-height:0}.cols3 .fill>.scroll{position:static;max-height:60vh}}
.info{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;font-size:14px}.info dt{color:var(--muted)}.info dd{margin:0;font-weight:600}
.yes{color:var(--ok)}.no{color:#9ca3af}
/* 지난 예약: 옅은 회색 */
.res.past{background:#d1d5db;color:#4b5563;box-shadow:none}.res.past .h::after{background:rgba(75,85,99,.5)}tr.past td{color:#9ca3af}
/* 예약 수정 모달 */
.modal{position:fixed;inset:0;background:rgba(17,24,39,.45);display:flex;align-items:center;justify-content:center;z-index:50;padding:16px}.modal[hidden]{display:none}
.modal-box{background:#fff;border-radius:12px;padding:20px 22px;width:100%;max-width:520px;box-shadow:0 10px 40px rgba(0,0,0,.3)}
.modal-box h2{margin:0 0 14px;font-size:17px}.modal-box .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.modal-box .grid .full{grid-column:1/-1}
.modal-box input[type=text],.modal-box input[type=date],.modal-box select{width:100%}
/* 설정 */
.chips{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0 12px}.chip{background:#eef2ff;border:1px solid #c7d2fe;border-radius:999px;padding:4px 10px;font-size:13px;display:flex;gap:6px;align-items:center}.chip b{cursor:pointer;color:var(--bad)}
/* 대시보드 */
.viz-root{--surface-1:#fcfcfb;--series-1:#2a78d6;--series-1-soft:#cde2fb;--grid:#e6e8eb;--text-secondary:#52514e}
.subtabs{display:flex;gap:6px;margin-bottom:14px}.subtab{padding:6px 14px;border-radius:999px;background:#e9edf2;cursor:pointer;font-size:13px;font-weight:600;color:var(--muted)}.subtab.active{background:var(--acc);color:#fff}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:16px}
.tile{background:var(--surface-1);border:1px solid var(--line);border-radius:10px;padding:12px 14px}.tile .lbl{font-size:12px;color:var(--muted)}.tile .val{font-size:26px;font-weight:600;margin-top:4px;font-variant-numeric:tabular-nums}.tile .sub{font-size:12px;color:var(--muted);margin-top:2px}
.tile.hero .val{font-size:40px}
.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:14px}
.chart{background:var(--surface-1);border:1px solid var(--line);border-radius:10px;padding:12px 14px;min-width:0}.chart h3{font-size:14px;margin:0 0 2px}.chart .sub{font-size:12px;color:var(--muted);margin-bottom:6px}
.chart svg{width:100%;height:190px;display:block;overflow:visible}.chart .bar{fill:var(--series-1);cursor:pointer}.chart .bar:hover{fill:#1c5cab}
.chart .grid{stroke:var(--grid);stroke-width:1}.chart .axis{fill:var(--text-secondary);font-size:11px}.chart .vl{fill:var(--fg);font-size:11px;font-weight:600}
.chart details{margin-top:6px}.chart summary{font-size:12px;color:var(--muted);cursor:pointer}.chart table td,.chart table th{padding:3px 6px;font-size:12px}
.viz-tip{position:fixed;z-index:60;background:#111827;color:#fff;font-size:12px;padding:5px 9px;border-radius:6px;pointer-events:none;white-space:nowrap;box-shadow:0 2px 8px rgba(0,0,0,.3)}
.logtable td{font-size:13px;vertical-align:top}.logtable .act{display:inline-block;padding:1px 8px;border-radius:999px;font-size:12px;font-weight:600;color:#fff}.act.c{background:var(--acc)}.act.u{background:var(--warn)}.act.d{background:var(--bad)}
.cfgtable input[type=text]{width:100%}.cfgtable input[type=number]{width:70px;padding:6px 8px;border:1px solid var(--line);border-radius:6px}.cfgtable select{padding:6px 8px}
.room{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:#fff;display:flex;flex-direction:column}
.room .img{background:#111;aspect-ratio:4/3;position:relative}.room .img img{width:100%;height:100%;object-fit:cover;display:block}
.room .img .noimg{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#9ca3af;font-size:13px}
.room .img .st{position:absolute;left:8px;top:8px}
/* 카드 본문: 왼쪽 텍스트(회의실명/상태) + 오른쪽 세로 버튼 3개 (회의실명 높이에서 시작) */
.room .body{padding:10px 12px;display:grid;grid-template-columns:1fr auto;gap:6px 12px;align-items:start}
.room .body .txt{display:grid;gap:6px;min-width:0}
/* 버튼 3개: 높이 약 2/3 로 줄이고, 회의실명 위쪽 ~ 마지막 줄(주소·갱신) 아래쪽 사이에 고르게 배치 -> 예약하기 버튼 아래가 텍스트 마지막 줄과 맞음 */
.room .body .btns{display:flex;flex-direction:column;justify-content:space-between;align-self:stretch;gap:4px;min-width:96px}
.room .body .btns a{display:block}.room .body .btns button{width:100%;padding:2px 8px;font-size:12px;line-height:1.35}
.room .name{font-weight:600;font-size:17px;text-align:center}
.room{cursor:pointer}.room:hover{border-color:var(--acc);box-shadow:0 2px 8px rgba(37,99,235,.15)}
.room.selected{outline:2px solid var(--acc)}
.tl-toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
.tl-wrap{width:100%;max-width:100%;overflow:hidden;border:1px solid var(--line);border-radius:8px;background:#fafbfc;user-select:none;-webkit-user-select:none}
.tl{position:relative;width:100%;height:112px}  /* 30분 칸 18개가 컨테이너 폭에 맞춰 균등 배치 - 좌우 스크롤 없음 */
.tl-hours{position:absolute;left:0;top:0;height:28px;width:100%}.tl-hours span{position:absolute;top:6px;font-size:12px;color:var(--muted);transform:translateX(-50%);white-space:nowrap}
.tl-hours span::after{content:"";position:absolute;left:50%;top:18px;width:1px;height:8px;background:var(--line)}
.tl-hours span.half{display:none}                       /* 30분 눈금 라벨은 숨기고 정시만 표시 */
.tl-hours span:last-child{transform:translateX(-100%)}  /* 마지막(20:00) 라벨이 오른쪽 밖으로 나가지 않게 */
.tl-hours span:first-child{transform:translateX(0)}
.tl-slots{position:absolute;left:0;top:30px;height:72px;width:100%;display:flex}
.slot{flex:1 1 0;min-width:0;height:100%;border-right:1px dashed var(--line);border-top:1px solid var(--line);border-bottom:1px solid var(--line);background:#fff;cursor:pointer;position:relative}
.slot:last-child{border-right:none}
@media (max-width:700px){.tl-hours span:not(.h3){display:none}.tl .res span:not(.x):not(.h){display:none}.tl .res{padding:4px 4px}}  /* 좁은 화면: 3시간 간격 라벨, 막대에는 이름만 */
.slot:nth-child(2n){background:#f8fafc}.slot:hover{background:#eef4ff}.slot.sel{background:var(--acc2)}.slot.busy{background:#f3f4f6;cursor:not-allowed}.slot.past{background:repeating-linear-gradient(45deg,#f6f7f8,#f6f7f8 6px,#eceef1 6px,#eceef1 12px)}
.slot small{display:none}
.res{position:absolute;top:36px;height:60px;border-radius:6px;background:#2563eb;color:#fff;padding:5px 8px;font-size:12px;overflow:hidden;cursor:grab;box-shadow:0 1px 3px rgba(0,0,0,.25);pointer-events:auto;touch-action:none}
.res.dragging{cursor:grabbing;opacity:.92;z-index:5;box-shadow:0 4px 12px rgba(0,0,0,.35);transition:none}.res.conflict{background:var(--bad)}.res.hit{outline:3px solid var(--bad);outline-offset:1px}
.res .h{position:absolute;top:0;bottom:0;width:10px;cursor:ew-resize;opacity:0}.res .hl{left:0}.res .hr{right:0}
.res .h::after{content:"";position:absolute;top:50%;width:3px;height:22px;margin-top:-11px;border-radius:2px;background:rgba(255,255,255,.85)}.res .hl::after{left:3px}.res .hr::after{right:3px}
.res:hover .h,.res.resizing .h{opacity:1}.res.resizing{cursor:ew-resize}
.drag-tip{position:absolute;z-index:6;top:4px;transform:translateX(-50%);background:#111827;color:#fff;font-size:12px;padding:4px 9px;border-radius:6px;white-space:nowrap;pointer-events:none;box-shadow:0 2px 6px rgba(0,0,0,.3)}.drag-tip.bad{background:var(--bad)}.res .x{position:absolute;right:4px;top:2px;font-size:13px;opacity:.7;cursor:pointer}.res .x:hover{opacity:1}
.res b{display:block;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.res span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;display:block;opacity:.9}
.res.now{background:#16a34a}
.tl-now{position:absolute;top:26px;bottom:6px;width:2px;background:var(--bad);z-index:3;pointer-events:none}.tl-now::before{content:"지금";position:absolute;top:-16px;left:-12px;font-size:10px;color:var(--bad)}
.datebox{position:relative;display:inline-block;padding:8px 12px;border:1px solid var(--line);border-radius:6px;background:#fff;font-weight:600;min-width:140px;cursor:pointer}
.datebox input[type=date]{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer;padding:0;border:0}
.datebox.sun #dateText{color:#dc2626}.datebox.sat #dateText{color:#2563eb}
.form-grid{display:flex;gap:10px;margin-top:12px;align-items:flex-end;flex-wrap:wrap}
.form-grid>div{flex:0 0 auto}.form-grid .grow{flex:1 1 90px;min-width:90px}.form-grid .grow2{flex:2 1 220px;min-width:180px}.form-grid .grow input{width:100%}
.form-grid .name{flex:0.6 1 60px;min-width:70px}.form-grid .purpose{flex:1.4 1 150px}   /* 예약자명 60%, 줄어든 만큼 회의 목적 확대 */
.form-grid .btn{margin-left:auto}
@media (max-width:700px){.form-grid .btn{margin-left:0;flex:1 1 100%}}
.msg{margin-top:8px;min-height:18px;font-size:13px}.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
.lnk,.ib{display:inline-flex;align-items:center;justify-content:center;width:22px;height:22px;border-radius:5px;text-decoration:none;font-size:12px;line-height:1;vertical-align:middle;padding:0;border:none;cursor:pointer}
.lnk.on{background:#dbeafe;color:var(--acc)}.lnk.on:hover{background:var(--acc);color:#fff}.lnk.off{color:#d1d5db;cursor:default;background:none}
.ib.edit{background:#e5e7eb;color:var(--fg)}.ib.edit:hover{background:#cbd5e1}.ib.del{background:#fee2e2;color:var(--bad)}.ib.del:hover{background:var(--bad);color:#fff}
.ib svg{width:13px;height:13px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.legend{display:flex;gap:12px;font-size:12px;color:var(--muted);align-items:center}.legend i{display:inline-block;width:12px;height:12px;border-radius:3px;margin-right:4px;vertical-align:-2px}
</style></head><body>
<header><h1>회의실 예약 시스템</h1><span class="muted" id="clock"></span><span class="sp"></span><a href="/admin">관리자</a></header>
<div class="tabs" id="tabs"></div>
<main>
<div id="viewRooms" style="display:grid;gap:16px;min-width:0">
<div class="cols3">
 <div class="card cams"><h2>회의실 현황 <span class="muted" id="roomsNote"></span></h2>
  <p class="muted" style="margin:-6px 0 10px{% if not detector_ok %};color:#dc2626{% endif %}">서버 검출: {{ detector }}{% if not detector_ok %} — 사람/얼굴 검출이 꺼져 있어 보드가 얼굴을 못 잡으면 "비어있음"으로 표시됩니다{% endif %}</p>
  <div class="fill"><div class="scroll"><div class="rooms" id="rooms"><div class="muted">인증된 카메라가 아직 없습니다. 기기에서 인증코드를 입력하면 여기에 표시됩니다.</div></div></div></div></div>
 <div class="card"><h2>회의실 정보 <span class="muted" id="infoName"></span></h2><div id="roomInfo" class="muted">회의실을 선택하세요</div></div>
 <div class="card"><h2>이 날의 예약 목록 <span class="muted" id="listNote"></span></h2>
  <div class="fill"><div class="scroll"><table><thead><tr><th>시간</th><th>예약자</th><th>회의 목적</th><th>링크</th><th></th></tr></thead><tbody id="resList"><tr><td colspan="5" class="muted">예약 없음</td></tr></tbody></table>
  <p class="muted" style="margin:10px 0 0">예약 막대를 더블클릭하거나 [수정]을 누르면 예약 정보를 고칠 수 있습니다</p></div></div></div>

<div class="card resv"><h2>예약</h2>
<div class="row">
 <div><label>회의실</label><select id="room" style="min-width:180px"></select></div>
 <div><label>날짜</label><div class="datebox"><span id="dateText"></span><input type="date" id="date" value="{{ today }}" title="클릭해서 날짜 선택"></div></div>
 <div style="align-self:end"><button class="sec" onclick="shiftDate(-1)">◀ 이전일</button> <button class="sec" onclick="setToday()">오늘</button> <button class="sec" onclick="shiftDate(1)">다음날 ▶</button></div>
</div>
<div class="tl-toolbar" style="margin-top:14px">
 <span class="muted">{{ '%02d' % open_hour }}:00 ~ {{ '%02d' % close_hour }}:00 · {{ slot_min }}분 단위 · 빈 칸을 클릭/드래그해서 시간 선택 · 예약 블록: 가운데 끌기=이동, 양끝 끌기=시간 조절, 더블클릭=수정, ✕=취소</span>
 <span class="sp" style="flex:1"></span>
 <span class="legend"><span><i style="background:#2563eb"></i>예약됨</span><span><i style="background:#16a34a"></i>진행 중</span><span><i style="background:#d1d5db"></i>지난 예약</span><span><i style="background:#dbeafe;border:1px solid #93c5fd"></i>선택</span></span>
</div>
<div class="tl-wrap" id="tlWrap"><div class="tl" id="tl"><div class="tl-hours" id="tlHours"></div><div class="tl-slots" id="tlSlots"></div><div id="tlRes"></div><div class="tl-now" id="tlNow" style="display:none"></div></div></div>

<div class="form-grid">
 <div><label>시작</label><select id="start"></select></div>
 <div><label>종료</label><select id="end"></select></div>
 <div class="grow name"><label>예약자명</label><input type="text" id="reserver" placeholder="홍길동"></div>
 <div class="grow purpose"><label>회의 목적</label><input type="text" id="purpose" placeholder="가급적 15자 이내로 입력요망"></div>
 <div class="grow grow2"><label>줌/구글밋 링크 <span class="muted">(선택)</span></label><input type="text" id="link" placeholder="https://zoom.us/j/... 또는 https://meet.google.com/..."></div>
 <div class="btn"><button id="btnReserve" onclick="reserve()">예약하기</button></div>
</div>
<div class="msg" id="msg"></div>
</div>
</div>
</div>

<div id="viewDash" class="card viz-root" hidden>
<div class="row" style="justify-content:space-between;margin-bottom:10px"><h2 style="margin:0">📊 대시보드</h2>
 <div class="subtabs" style="margin:0"><span class="subtab active" data-sub="stats" onclick="setDashSub('stats')">통계</span><span class="subtab" data-sub="log" onclick="setDashSub('log')">로그</span></div></div>
<div id="dashStats">
 <div class="row" style="margin-bottom:12px"><span class="muted">기간</span>
  <select id="statPeriod" onchange="loadStats()"><option value="all">전체</option><option value="30">최근 30일</option><option value="90">최근 90일</option><option value="year">올해</option></select>
  <span class="muted" id="statNote"></span></div>
 <div class="tiles" id="tiles"></div>
 <div class="charts">
  <div class="chart"><h3>요일별 회의 수</h3><div class="sub">선택한 기간의 예약 건수</div><div id="chDow"></div></div>
  <div class="chart"><h3>시간대별 회의 빈도</h3><div class="sub">그 시간에 진행 중이던 회의 수</div><div id="chHour"></div></div>
  <div class="chart"><h3>월별 회의 수</h3><div class="sub">최근 12개월</div><div id="chMonth"></div></div>
  <div class="chart"><h3>회의실별 회의 수</h3><div class="sub">상위 8개</div><div id="chRoom"></div></div>
 </div>
</div>
<div id="dashLog" hidden>
 <div class="row" style="margin-bottom:10px"><span class="muted">예약 / 수정 / 삭제 활동 기록 (최신순)</span>
  <select id="logFilter" onchange="renderLog()"><option value="">전체</option><option value="예약">예약</option><option value="수정">수정</option><option value="삭제">삭제</option></select>
  <button class="sm sec" onclick="loadLog()">새로고침</button></div>
 <div style="overflow-x:auto"><table class="logtable"><thead><tr><th>시각</th><th>동작</th><th>회의실</th><th>회의 날짜·시간</th><th>예약자</th><th>회의 목적</th><th>상세</th><th>IP</th></tr></thead><tbody id="logBody"><tr><td colspan="8" class="muted">기록 없음</td></tr></tbody></table></div>
</div>
</div>

<div id="viewSettings" class="card" hidden>
<h2>⚙ 설정 — 층 / 회의실 기본 정보</h2>
<h3 style="font-size:14px;margin:6px 0">층 목록</h3>
<div class="chips" id="floorChips"></div>
<div class="row"><input type="text" id="newFloor" placeholder="예: 8층" style="max-width:160px"><button class="sec" onclick="addFloor()">층 추가</button></div>
<h3 style="font-size:14px;margin:18px 0 6px">회의실 기본 정보</h3>
<p class="muted" style="margin:0 0 8px">카메라 기기·예약에 등장한 회의실은 자동으로 나타납니다. 층을 지정하면 해당 층 탭에 표시됩니다.</p>
<div style="overflow-x:auto"><table class="cfgtable"><thead><tr><th>회의실명</th><th>층</th><th title="접속 시 이 회의실의 층 탭이 자동으로 열립니다">자주 이용하는 회의실</th><th>TV</th><th>의자 수</th><th>테이블 수</th><th>화이트보드</th><th>화상회의 장비</th><th>비고</th><th></th></tr></thead><tbody id="cfgRooms"></tbody></table></div>
<div class="row" style="margin-top:10px"><input type="text" id="newRoom" placeholder="회의실명 직접 추가" style="max-width:220px"><button class="sec" onclick="addRoomRow()">회의실 추가</button></div>
<p class="muted" style="margin:8px 0 0">자주 이용하는 회의실: 표에서 하나를 선택하면 접속 시 그 회의실의 층 탭이 자동으로 열리고 그 회의실이 선택됩니다. <a href="#" onclick="cfgEdit.favorite='';renderSettings();return false">선택 해제</a></p>
<div class="row" style="margin-top:16px"><button onclick="saveConfig()">설정 저장</button><span class="msg" id="cfgMsg" style="margin:0"></span></div>
</div>
</main>

<div id="editModal" class="modal" hidden>
 <div class="modal-box">
  <h2>예약 수정</h2>
  <div class="grid">
   <div><label>회의실</label><input type="text" id="eRoom" disabled></div>
   <div><label>날짜</label><input type="date" id="eDate"></div>
   <div><label>시작</label><select id="eStart"></select></div>
   <div><label>종료</label><select id="eEnd"></select></div>
   <div><label>예약자명</label><input type="text" id="eReserver"></div>
   <div><label>회의 목적</label><input type="text" id="ePurpose"></div>
   <div class="full"><label>줌/구글밋 링크 <span class="muted">(선택)</span></label><input type="text" id="eLink" placeholder="https://..."></div>
  </div>
  <div class="msg" id="eMsg"></div>
  <div class="row" style="margin-top:12px;justify-content:flex-end"><button class="bad" onclick="deleteFromEdit()">예약 삭제</button><span class="sp" style="flex:1"></span><button class="sec" onclick="closeEdit()">취소</button><button onclick="confirmEdit()">수정 확인</button></div>
 </div>
</div>
<script>
const OPEN={{ open_hour }}, CLOSE={{ close_hour }}, STEP={{ slot_min }};
const SERVER_BLUR={{ 'true' if server_blur else 'false' }};
const NSLOTS=(CLOSE-OPEN)*60/STEP;
const $=id=>document.getElementById(id);
const wrap=$('tlWrap');
const pct=slots=>(slots/NSLOTS*100)+'%';   // 칸 수 -> 가로 위치/폭 (%). 스크롤 없이 컨테이너 폭에 맞춤
const hhmm=m=>String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');
const slotMin=i=>OPEN*60+i*STEP;
let reservations=[], selStart=-1, selEnd=-1, dragging=false, rooms=[];
let config={floors:[],rooms:{},favorite:''}, currentTab='all', editId=null, startedAtFavorite=false, loadedOnce=false;

// ---- 시간 눈금 / 칸 생성 ----
(function build(){
  const hours=$('tlHours');
  for(let i=0;i<=NSLOTS;i++){const s=document.createElement('span');s.style.left=pct(i);s.textContent=hhmm(slotMin(i));if(slotMin(i)%60!==0)s.className='half';else if((slotMin(i)/60-OPEN)%3===0)s.className='h3';hours.appendChild(s)}
  const slots=$('tlSlots');
  for(let i=0;i<NSLOTS;i++){const d=document.createElement('div');d.className='slot';d.dataset.i=i;d.innerHTML='<small>'+hhmm(slotMin(i))+'</small>';slots.appendChild(d)}
  for(let i=0;i<=NSLOTS;i++){const t=hhmm(slotMin(i));if(i<NSLOTS){$('start').add(new Option(t,t));$('eStart').add(new Option(t,t))}if(i>0){$('end').add(new Option(t,t));$('eEnd').add(new Option(t,t))}}
  $('start').onchange=()=>{selStart=Math.max(0,($('start').value.split(':')[0]*60+ +$('start').value.split(':')[1]-OPEN*60)/STEP);if($('end').selectedIndex<selStart+0)$('end').selectedIndex=selStart;selEnd=$('end').selectedIndex;paintSel()};
  $('end').onchange=()=>{selEnd=$('end').selectedIndex;if(selEnd<selStart)selEnd=selStart;paintSel()};
})();

// ---- 칸 클릭/드래그로 시간 범위 선택 ----
wrap.addEventListener('mousedown',e=>{
  const slot=e.target.closest('.slot');
  if(slot&&!slot.classList.contains('busy')&&!slot.classList.contains('past')){ // 칸 드래그 = 범위 선택
    dragging=true;selStart=selEnd=+slot.dataset.i;paintSel();e.preventDefault()}
});
window.addEventListener('mousemove',e=>{
  if(dragging){const slot=document.elementFromPoint(e.clientX,e.clientY)?.closest('.slot');if(slot)extendSel(+slot.dataset.i)}
});
window.addEventListener('mouseup',()=>{if(dragging){dragging=false;syncForm()}});
// 터치: 칸 탭 = 시작 지정 후 다시 탭 = 종료 지정
wrap.addEventListener('click',e=>{const slot=e.target.closest('.slot');if(!slot||!('ontouchstart' in window))return;if(slot.classList.contains('busy')||slot.classList.contains('past'))return;
  const i=+slot.dataset.i;if(selStart<0||i<selStart||selEnd>selStart){selStart=selEnd=i}else{extendSel(i)}paintSel();syncForm()});
// ---- 예약 블록을 좌우로 끌어 시간 변경 (30분 단위 스냅, 놓으면 서버에 반영) ----
let drag=null;
const slotPx=()=>$('tl').getBoundingClientRect().width/NSLOTS;
const overlapWith=(id,s,e)=>reservations.find(r=>r.id!==id&&toMin(r.start)<e&&s<toMin(r.end));   // 겹치는 기존 예약 (없으면 undefined)
const overlaps=(id,s,e)=>!!overlapWith(id,s,e);
const resLabel=r=>r.start+'~'+r.end+' '+r.reserver+(r.purpose?' ('+r.purpose+')':'');
function showTip(el,text,bad){let t=$('dragTip');if(!t){t=document.createElement('div');t.id='dragTip';t.className='drag-tip';$('tl').appendChild(t)}
  t.textContent=text;t.className='drag-tip'+(bad?' bad':'');const tl=$('tl').getBoundingClientRect(),r=el.getBoundingClientRect();t.style.left=(r.left-tl.left+r.width/2)+'px';t.style.display=''}
function hideTip(){const t=$('dragTip');if(t)t.style.display='none';document.querySelectorAll('.res.hit').forEach(e=>e.classList.remove('hit'))}
// mode: 'move'(통째로 이동) / 'start'(시작 시간 조절) / 'end'(종료 시간 조절)
function pickMode(el,target,x){if(target.closest('.hl'))return 'start';if(target.closest('.hr'))return 'end';
  const r=el.getBoundingClientRect(),edge=Math.min(14,r.width/4);   // 손잡이가 아니어도 가장자리 근처면 크기 조절
  if(x-r.left<edge)return 'start';if(r.right-x<edge)return 'end';return 'move'}
function beginDrag(el,x,mode){const res=reservations.find(r=>r.id===el.dataset.id);if(!res)return;
  const s0=(toMin(res.start)-OPEN*60)/STEP,e0=(toMin(res.end)-OPEN*60)/STEP;
  drag={id:res.id,res,el,mode,s0,e0,s:s0,e:e0,x0:x,moved:false};el.classList.add('dragging');el.classList.add(mode==='move'?'moving':'resizing');
  showTip(el,res.start+' ~ '+res.end+(mode==='move'?'  (좌우로 끌어 이동)':mode==='start'?'  (시작 시간 조절)':'  (종료 시간 조절)'),false)}
function applyDrag(){const d=drag,s=slotMin(d.s),e=slotMin(d.e),hit=overlapWith(d.id,s,e),bad=!!hit;
  d.el.style.left='calc('+pct(d.s)+' + 2px)';d.el.style.width='calc('+pct(d.e-d.s)+' - 4px)';
  d.el.querySelector('.t').textContent=hhmm(s)+'~'+hhmm(e);d.el.classList.toggle('conflict',bad);
  document.querySelectorAll('.res.hit').forEach(x=>x.classList.remove('hit'));if(hit){const h=document.querySelector('.res[data-id="'+hit.id+'"]');if(h)h.classList.add('hit')}
  const dur=(d.e-d.s)*STEP,durTxt=(dur>=60?Math.floor(dur/60)+'시간':'')+(dur%60?' '+dur%60+'분':'');
  showTip(d.el,bad?'⚠ '+resLabel(hit)+' 예약과 겹칩니다':hhmm(s)+' ~ '+hhmm(e)+' ('+durTxt.trim()+')',bad);
  const m=$('msg');m.className='msg'+(bad?' bad':'');m.textContent=(bad?'겹침: '+resLabel(hit)+' → ':(d.mode==='move'?'이동 중: ':'시간 조절 중: '))+hhmm(s)+' ~ '+hhmm(e)}
function moveDrag(x){if(!drag)return;if(Math.abs(x-drag.x0)>4)drag.moved=true;
  const d=drag,delta=Math.round((x-d.x0)/slotPx());let ns=d.s,ne=d.e;
  if(d.mode==='move'){const dur=d.e0-d.s0;ns=Math.max(0,Math.min(NSLOTS-dur,d.s0+delta));ne=ns+dur}
  else if(d.mode==='start'){ns=Math.max(0,Math.min(d.e0-1,d.s0+delta))}          // 최소 30분 유지
  else{ne=Math.max(d.s0+1,Math.min(NSLOTS,d.e0+delta))}
  if(ns===d.s&&ne===d.e)return;d.s=ns;d.e=ne;applyDrag()}
async function moveRes(id,start,end){   // 서버에 시간 변경 요청. 성공 시 true
  const r=await fetch('/api/reservations/'+id,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({start,end})});const j=await r.json();
  if(!j.ok)throw new Error(j.error||'변경 실패');return true}
async function endDrag(){if(!drag)return;const d=drag;drag=null;d.el.classList.remove('dragging','moving','resizing');hideTip();
  const m=$('msg');
  if(!d.moved||(d.s===d.s0&&d.e===d.e0)){m.className='msg';m.textContent='';if(d.s!==d.s0||d.e!==d.e0)render();return}
  const s=slotMin(d.s),e=slotMin(d.e),hit=overlapWith(d.id,s,e);
  if(hit){render();m.className='msg bad';m.textContent='기존 예약과 겹쳐서 변경되지 않았습니다: '+resLabel(hit);
    alert('기존 예약과 겹칩니다.\n\n  '+resLabel(hit)+'\n\n'+d.res.reserver+' 예약은 '+d.res.start+'~'+d.res.end+' 그대로 유지됩니다.');return}
  const what=d.mode==='move'?'이동':d.mode==='start'?'시작 시간 변경':'종료 시간 변경';
  try{await moveRes(d.id,hhmm(s),hhmm(e));
    m.className='msg ok';m.innerHTML=esc(d.res.reserver)+' 예약 '+what+': '+d.res.start+'~'+d.res.end+' → <b>'+hhmm(s)+' ~ '+hhmm(e)+'</b> ';
    const u=document.createElement('button');u.className='sm sec';u.textContent='되돌리기';u.onclick=async()=>{try{await moveRes(d.id,d.res.start,d.res.end);m.className='msg';m.textContent='원래 시간('+d.res.start+'~'+d.res.end+')으로 되돌렸습니다'}catch(err){alert(err.message)}loadReservations();loadRooms()};m.appendChild(u)}
  catch(err){render();m.className='msg bad';m.textContent='변경 실패: '+err.message;alert('변경할 수 없습니다.\n\n'+err.message)}
  loadReservations();loadRooms()}
let lastPress={id:null,t:0};
function isDoublePress(id){const now=Date.now();const dbl=lastPress.id===id&&now-lastPress.t<400;lastPress={id:dbl?null:id,t:now};return dbl}
$('tlRes').addEventListener('mousedown',e=>{if(e.button!==0)return;const el=e.target.closest('.res');if(!el||e.target.closest('.x'))return;
  if(isDoublePress(el.dataset.id)){if(drag){drag=null;el.classList.remove('dragging','moving','resizing');hideTip()}openEdit(el.dataset.id);e.preventDefault();return}   // 더블클릭 -> 수정 창
  beginDrag(el,e.clientX,pickMode(el,e.target,e.clientX));e.preventDefault()});
window.addEventListener('mousemove',e=>{if(drag)moveDrag(e.clientX)});
window.addEventListener('mouseup',()=>{if(drag)endDrag()});
$('tlRes').addEventListener('touchstart',e=>{const el=e.target.closest('.res');if(!el||e.target.closest('.x'))return;
  if(isDoublePress(el.dataset.id)){if(drag){drag=null;el.classList.remove('dragging','moving','resizing');hideTip()}openEdit(el.dataset.id);e.preventDefault();return}
  const x=e.touches[0].clientX;beginDrag(el,x,pickMode(el,e.target,x));e.preventDefault()},{passive:false});
window.addEventListener('touchmove',e=>{if(drag){moveDrag(e.touches[0].clientX);e.preventDefault()}},{passive:false});
window.addEventListener('touchend',()=>{if(drag)endDrag()});
// ✕ 버튼: 예약 취소
$('tlRes').addEventListener('click',e=>{const x=e.target.closest('.x');if(!x)return;const el=x.closest('.res');const res=reservations.find(r=>r.id===el.dataset.id);if(!res)return;
  if(confirm(res.start+'~'+res.end+' '+res.reserver+' ('+res.purpose+')\n이 예약을 취소할까요?'))delRes(res.id)});

function extendSel(i){ // selStart 부터 i 까지, 중간에 예약된 칸이 있으면 그 앞까지만
  if(i<selStart){selStart=i;return paintSel()}
  let j=selStart;while(j<i&&!isBusy(j+1))j++;selEnd=j;paintSel()}
function isBusy(i){const s=slotMin(i),e=s+STEP;return reservations.some(r=>toMin(r.start)<e&&s<toMin(r.end))}
const toMin=t=>+t.split(':')[0]*60+ +t.split(':')[1];
function paintSel(){document.querySelectorAll('.slot').forEach(s=>{const i=+s.dataset.i;s.classList.toggle('sel',selStart>=0&&i>=selStart&&i<=selEnd)})}
function syncForm(){if(selStart<0)return;$('start').value=hhmm(slotMin(selStart));$('end').value=hhmm(slotMin(selEnd+1))}

// ---- 데이터 ----
async function loadRooms(){
  const j=await(await fetch('/api/rooms',{cache:'no-store'})).json();rooms=j.rooms;
  const camRooms=rooms.map(r=>r.room);   // 카메라가 있는 회의실을 먼저, 나머지는 이름순
  const names=(await(await fetch('/api/room-names')).json()).rooms.filter(inTab).sort((a,b)=>(camRooms.includes(b)-camRooms.includes(a))||a.localeCompare(b,'ko'));
  const sel=$('room'),cur=sel.value;sel.innerHTML='';names.forEach(n=>sel.add(new Option(n,n)));
  if(names.includes(cur))sel.value=cur;else if(names.length)sel.value=names[0];
  if(sel.value!==cur||!loadedOnce){loadedOnce=true;loadReservations()}else renderInfo();
  const box=$('rooms');const shown=rooms.filter(r=>inTab(r.room));
  if(!shown.length){box.innerHTML='<div class="muted">'+(rooms.length?'이 층에 카메라가 등록된 회의실이 없습니다. ⚙ 설정에서 회의실의 층을 지정하세요.':'인증된 카메라가 아직 없습니다. 기기에서 인증코드를 입력하면 여기에 표시됩니다.')+'</div>';$('roomsNote').textContent='· '+j.server_time;return}
  const t=Date.now();
  box.innerHTML=shown.map(r=>{
    const st=!r.online?'<span class="badge">오프라인</span>':r.occupied?'<span class="badge ok">재실 ('+(r.occupied_by_server?'서버 검출 ':'')+r.faces+'명)</span>':'<span class="badge warn">비어있음</span>';
    const cr=r.current_reservation;
    let note='';
    if(r.online&&cr&&!r.occupied)note='<span class="muted">예약 시간이지만 비어있음</span>';
    if(r.online&&!cr&&r.occupied)note='<span class="muted">예약 없이 사용 중</span>';
    let warn='';if(r.online&&r.detect===false)warn='<div style="color:#dc2626;font-size:12px">이 기기는 얼굴 감지(블러)가 꺼져 있습니다 - 보드 PSRAM 설정 확인'+(SERVER_BLUR?' (서버에서 대신 블러 처리 중)':' (서버 블러도 꺼짐: opencv 설치 필요)')+'</div>';
    warn = '';
    return '<div class="room'+(r.room===sel.value?' selected':'')+'" data-room="'+esc(r.room)+'" onclick="if(!event.target.closest(\'a,button\'))pickRoom(this.dataset.room)"><div class="img">'+(r.has_snapshot?'<img src="/snapshot/'+r.mac+'.jpg?t='+t+'">':'<div class="noimg">스냅샷 없음</div>')+'<div class="st">'+st+'</div></div>'
     +'<div class="body"><div class="txt"><div class="name">'+esc(r.room||'(회의실명 없음)')+'</div>'
     +'<div class="muted">'+(cr?'현재 예약: '+cr.start+'~'+cr.end+' '+esc(cr.reserver)+' · '+esc(cr.purpose):'현재 예약 없음')+(note?' &nbsp;|&nbsp; '+note:'')+'</div>'+warn
     +'<div class="muted">'+esc(r.ip)+' · 갱신 '+(r.last_heartbeat_ago==null?'-':r.last_heartbeat_ago+'초 전')+(r.rssi?' · '+r.rssi+' dBm':'')+'</div></div>'
     +'<div class="btns"><a href="'+r.stream_url+'" target="_blank"><button class="sec" type="button">실시간 영상</button></a><a href="'+r.device_url+'" target="_blank"><button class="sec" type="button">기기 페이지</button></a><button onclick="pickRoom(\''+esc(r.room)+'\')">예약하기</button></div></div></div>'}).join('');
  $('roomsNote').textContent='· '+j.server_time;fitCols();
}
function fitCols(){}   // 왼쪽 카드 높이는 grid 가 오른쪽(정보/목록 + 예약) 블록 높이에 맞춰 늘림
window.addEventListener('resize',()=>{if(currentTab==='dash')loadStats()});
function pickRoom(n){$('room').value=n;loadReservations();window.scrollTo({top:$('room').getBoundingClientRect().top+window.scrollY-80,behavior:'smooth'})}
// 날짜를 "2026.09.20.일" 형식으로 표시 (input[type=date] 는 표시 형식을 바꿀 수 없어 텍스트로 대신 보여줌)
function updateDow(){const v=$('date').value;const box=$('dateText').parentElement;if(!v){$('dateText').textContent='';return}
  const p=v.split('-');const d=new Date(+p[0],+p[1]-1,+p[2]);const n=['일','월','화','수','목','금','토'][d.getDay()];
  $('dateText').textContent=p[0]+'.'+p[1]+'.'+p[2]+'.'+n;box.className='datebox'+(d.getDay()===0?' sun':d.getDay()===6?' sat':'')}
async function loadReservations(){updateDow();
  const room=$('room').value,d=$('date').value;
  document.querySelectorAll('.room').forEach(el=>el.classList.toggle('selected',el.querySelector('.name')?.textContent===room));
  renderInfo();$('listNote').textContent=room?'· '+room+' · '+$('dateText').textContent:'';
  if(!room){reservations=[];render();return}
  const j=await(await fetch('/api/reservations?room='+encodeURIComponent(room)+'&date='+d,{cache:'no-store'})).json();
  reservations=j.reservations;render();
  if(selStart<0)applyDefaultTime();   // 사용자가 칸을 고르기 전이면 현재 시각 기준 기본 시간 선택
}
// 시작 시간 기본값: 오늘이면 현재 시각 이후의 다음 30분 경계 (16:35 -> 17:00), 종료는 +30분. 다른 날은 08:00
function applyDefaultTime(){const now=new Date();let m=OPEN*60;
  if($('date').value===ymd(now)){m=Math.ceil((now.getHours()*60+now.getMinutes()+1)/STEP)*STEP;m=Math.max(OPEN*60,Math.min(CLOSE*60-STEP,m))}
  const i=(m-OPEN*60)/STEP;selStart=i;selEnd=i;
  // 그 칸이 이미 예약돼 있으면 다음 빈 칸으로
  while(selStart<NSLOTS-1&&isBusy(selStart)){selStart++;selEnd=selStart}
  $('start').value=hhmm(slotMin(selStart));$('end').value=hhmm(slotMin(selEnd+1));paintSel()}
function render(){
  if(typeof drag!=='undefined'&&drag)return;   // 드래그 중에는 다시 그리지 않음
  const now=new Date(),d=$('date').value,todayStr=ymd(now);
  const isToday=d===todayStr,nowMin=now.getHours()*60+now.getMinutes();
  const isPast=d<todayStr;
  document.querySelectorAll('.slot').forEach(s=>{const i=+s.dataset.i;s.classList.toggle('busy',isBusy(i));s.classList.toggle('past',isPast||(isToday&&slotMin(i)+STEP<=nowMin))});
  $('tlRes').innerHTML=reservations.map(r=>{const l=(toMin(r.start)-OPEN*60)/STEP,w=(toMin(r.end)-toMin(r.start))/STEP;
    const cur=isToday&&toMin(r.start)<=nowMin&&nowMin<toMin(r.end);
    const past=isPast||(isToday&&toMin(r.end)<=nowMin);   // 끝난 예약은 옅은 회색
    return '<div class="res'+(cur?' now':'')+(past?' past':'')+'" data-id="'+r.id+'" style="left:calc('+pct(l)+' + 2px);width:calc('+pct(w)+' - 4px)" title="가운데: 이동 / 양끝: 시간 조절 · '+esc(r.purpose)+(r.link?' · 링크: '+esc(r.link):'')+'"><b>'+esc(r.reserver)+'</b><span class="t">'+r.start+'~'+r.end+'</span><span>'+esc(r.purpose)+'</span><span class="x" title="예약 취소">✕</span><span class="h hl" title="시작 시간 조절"></span><span class="h hr" title="종료 시간 조절"></span></div>'}).join('');
  const nowEl=$('tlNow');
  if(isToday&&nowMin>=OPEN*60&&nowMin<=CLOSE*60){nowEl.style.display='';nowEl.style.left=pct((nowMin-OPEN*60)/STEP)}else nowEl.style.display='none';
  paintSel();
  $('resList').innerHTML=reservations.length?reservations.map(r=>{const past=isPast||(isToday&&toMin(r.end)<=nowMin);const cur=isToday&&toMin(r.start)<=nowMin&&nowMin<toMin(r.end);
    return '<tr'+(past?' class="past"':cur?' class="now"':'')+'><td style="white-space:nowrap">'+r.start+' ~ '+r.end+(cur?'<span class="badge ok">진행중</span>':'')+'</td><td>'+esc(r.reserver)+'</td><td>'+esc(r.purpose)+'</td><td>'+linkIcon(r.link)+'</td><td>'+ICON_EDIT.replace('%ID%',r.id)+' '+ICON_DEL.replace('%ID%',r.id)+'</td></tr>'}).join(''):'<tr><td colspan="5" class="muted">예약 없음</td></tr>';
}
async function reserve(){
  const m=$('msg');m.className='msg';m.textContent='';
  const body={room:$('room').value,date:$('date').value,start:$('start').value,end:$('end').value,reserver:$('reserver').value,purpose:$('purpose').value,link:$('link').value};
  if(!body.room){m.className='msg bad';m.textContent='회의실을 선택하세요';return}
  $('btnReserve').disabled=true;
  try{const r=await fetch('/api/reservations',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const j=await r.json();
    if(j.ok){m.className='msg ok';m.textContent='예약되었습니다: '+body.start+'~'+body.end+' '+body.reserver;selStart=selEnd=-1;$('purpose').value='';$('link').value='';loadReservations();loadRooms()}
    else{m.className='msg bad';m.textContent=j.error||'예약 실패'}
  }catch(e){m.className='msg bad';m.textContent='요청 실패: '+e}
  $('btnReserve').disabled=false;
}
async function delRes(id){const r=await fetch('/api/reservations/'+id,{method:'DELETE'});const j=await r.json();if(!j.ok)alert(j.error||'취소 실패');loadReservations();loadRooms()}
// toISOString() 은 UTC 기준이라 한국(UTC+9)에서는 날짜가 하루 어긋남 -> 로컬 날짜로 직접 포맷
function ymd(d){return d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')}
function shiftDate(n){const p=($('date').value||ymd(new Date())).split('-');const d=new Date(+p[0],+p[1]-1,+p[2]);d.setDate(d.getDate()+n);$('date').value=ymd(d);selStart=selEnd=-1;loadReservations()}
function setToday(){$('date').value=ymd(new Date());selStart=selEnd=-1;loadReservations()}
$('room').onchange=()=>{selStart=selEnd=-1;loadReservations()};
$('date').onchange=()=>{selStart=selEnd=-1;loadReservations()};

// ---- 탭 (전체 / 층 / 설정) ----
const inTab=name=>currentTab==='all'||(config.rooms[name]||{}).floor===currentTab;
async function loadConfig(){try{config=await(await fetch('/api/config',{cache:'no-store'})).json()}catch(e){}
  if(!startedAtFavorite){startedAtFavorite=true;const fav=config.favorite;
    if(fav&&config.rooms[fav]){const fl=config.rooms[fav].floor;currentTab=(fl&&config.floors.includes(fl))?fl:'all';$('room').innerHTML='';$('room').add(new Option(fav,fav));$('room').value=fav}}
  renderTabs()}
function renderTabs(){const t=$('tabs');const tabs=[['all','전체'],...config.floors.map(f=>[f,f]),['dash','대시보드'],['settings','⚙ 설정']];
  if(!tabs.some(x=>x[0]===currentTab))currentTab='all';
  t.innerHTML=tabs.map(([id,label])=>(id==='dash'?'<div class="tabclock" id="tabClock"></div>':'')+'<div class="tab'+(id===currentTab?' active':'')+(id==='settings'||id==='dash'?' cfg':'')+'" data-tab="'+esc(id)+'">'+esc(label)+'</div>').join('');tick();
  t.querySelectorAll('.tab').forEach(el=>el.onclick=()=>setTab(el.dataset.tab))}
function setTab(id){currentTab=id;renderTabs();const st=id==='settings',dash=id==='dash';$('viewSettings').hidden=!st;$('viewDash').hidden=!dash;$('viewRooms').style.display=(st||dash)?'none':'';
  if(st)renderSettings();else if(dash){loadStats();loadLog()}else{$('room').value='';loadRooms()}}

// ---- 가운데: 회의실 기본 정보 ----
function renderInfo(){const name=$('room').value;$('infoName').textContent=name?'· '+name:'';const box=$('roomInfo');
  if(!name){box.className='muted';box.textContent='회의실을 선택하세요';return}
  const i=config.rooms[name]||{};const cam=rooms.find(r=>r.room===name);
  const yn=v=>v?'<span class="yes">있음</span>':'<span class="no">없음</span>';
  let st='<span class="no">카메라 없음</span>';
  if(cam)st=!cam.online?'<span class="badge">오프라인</span>':cam.occupied?'<span class="badge ok">재실 ('+cam.faces+'명)</span>':'<span class="badge warn">비어있음</span>';
  const cr=cam&&cam.current_reservation;
  box.className='';box.innerHTML='<dl class="info">'
   +'<dt>층</dt><dd>'+(i.floor?esc(i.floor):'<span class="no">미지정</span>')+'</dd>'
   +'<dt>TV</dt><dd>'+yn(i.tv)+'</dd>'
   +'<dt>의자 수</dt><dd>'+(i.chairs||0)+'개</dd>'
   +'<dt>테이블 수</dt><dd>'+(i.tables||0)+'개</dd>'
   +'<dt>화이트보드</dt><dd>'+yn(i.whiteboard)+'</dd>'
   +'<dt>화상회의 장비</dt><dd>'+yn(i.vc)+'</dd>'
   +'<dt>비고</dt><dd>'+(i.note?esc(i.note):'<span class="no">-</span>')+'</dd>'
   +'<dt>현재 상태</dt><dd>'+st+'</dd>'
   +'<dt>현재 예약</dt><dd>'+(cr?cr.start+' ~ '+cr.end:'<span class="no">없음</span>')+'</dd>'
   +'<dt>예약자</dt><dd>'+(cr?esc(cr.reserver):'<span class="no">-</span>')+'</dd>'
   +'<dt>회의 목적</dt><dd>'+(cr?esc(cr.purpose):'<span class="no">-</span>')+'</dd>'
   +'</dl><p class="muted" style="margin:12px 0 0"><a href="#" onclick="setTab(\'settings\');return false">⚙ 설정에서 정보 수정</a></p>'}

// ---- 예약 수정 모달 (막대 더블클릭 / 목록의 [수정]) ----
function openEdit(id){const r=reservations.find(x=>x.id===id);if(!r)return;editId=id;
  $('eRoom').value=r.room;$('eDate').value=r.date;$('eStart').value=r.start;$('eEnd').value=r.end;$('eReserver').value=r.reserver;$('ePurpose').value=r.purpose;$('eLink').value=r.link||'';
  $('eMsg').className='msg';$('eMsg').textContent='';$('editModal').hidden=false;$('eReserver').focus()}
function closeEdit(){$('editModal').hidden=true;editId=null}
async function confirmEdit(){if(!editId)return;const m=$('eMsg');m.className='msg';m.textContent='';
  const body={date:$('eDate').value,start:$('eStart').value,end:$('eEnd').value,reserver:$('eReserver').value.trim(),purpose:$('ePurpose').value.trim(),link:$('eLink').value.trim()};
  if(toMin(body.start)>=toMin(body.end)){m.className='msg bad';m.textContent='종료 시간은 시작 시간보다 늦어야 합니다';return}
  if(!body.reserver||!body.purpose){m.className='msg bad';m.textContent='예약자명과 회의 목적을 입력하세요';return}
  try{const r=await fetch('/api/reservations/'+editId,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const j=await r.json();
    if(!j.ok){m.className='msg bad';m.textContent=j.error||'수정 실패';return}
    closeEdit();const mm=$('msg');mm.className='msg ok';mm.textContent='예약을 수정했습니다: '+body.date+' '+body.start+'~'+body.end+' '+body.reserver;
    if(body.date!==$('date').value){$('date').value=body.date}   // 날짜를 옮겼으면 그 날짜로 이동
    loadReservations();loadRooms()}
  catch(e){m.className='msg bad';m.textContent='요청 실패: '+e}}
async function deleteFromEdit(){if(!editId)return;const r=reservations.find(x=>x.id===editId);
  if(confirm((r?r.start+'~'+r.end+' '+r.reserver+'\n':'')+'이 예약을 삭제할까요?')){const id=editId;closeEdit();delRes(id)}}
$('tlRes').addEventListener('dblclick',e=>{const el=e.target.closest('.res');if(el)openEdit(el.dataset.id)});
$('editModal').addEventListener('click',e=>{if(e.target===$('editModal'))closeEdit()});
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&!$('editModal').hidden)closeEdit()});

// ---- 설정 탭: 층 / 회의실 기본 정보 ----
let cfgEdit=null;   // 편집 중인 사본
function renderSettings(){if(!cfgEdit)cfgEdit=JSON.parse(JSON.stringify(config));
  $('floorChips').innerHTML=cfgEdit.floors.length?cfgEdit.floors.map((f,i)=>'<span class="chip">'+esc(f)+'<b title="삭제" onclick="removeFloor('+i+')">✕</b></span>').join(''):'<span class="muted">등록된 층이 없습니다</span>';
  const opts=f=>'<option value="">미지정</option>'+cfgEdit.floors.map(x=>'<option'+(x===f?' selected':'')+'>'+esc(x)+'</option>').join('');
  const names=Object.keys(cfgEdit.rooms).sort();
  $('cfgRooms').innerHTML=names.length?names.map(n=>{const i=cfgEdit.rooms[n];const d='data-n="'+esc(n)+'"';
    return '<tr><td><b>'+esc(n)+'</b></td>'
     +'<td><select '+d+' data-k="floor">'+opts(i.floor)+'</select></td>'
     +'<td><input type="radio" name="fav" value="'+esc(n)+'"'+(cfgEdit.favorite===n?' checked':'')+' onchange="cfgEdit.favorite=this.value" title="자주 이용하는 회의실로 지정"></td>'
     +'<td><input type="checkbox" '+d+' data-k="tv"'+(i.tv?' checked':'')+'></td>'
     +'<td><input type="number" min="0" '+d+' data-k="chairs" value="'+(i.chairs||0)+'"></td>'
     +'<td><input type="number" min="0" '+d+' data-k="tables" value="'+(i.tables||0)+'"></td>'
     +'<td><input type="checkbox" '+d+' data-k="whiteboard"'+(i.whiteboard?' checked':'')+'></td>'
     +'<td><input type="checkbox" '+d+' data-k="vc"'+(i.vc?' checked':'')+'></td>'
     +'<td><input type="text" '+d+' data-k="note" value="'+esc(i.note||'')+'"></td>'
     +'<td><button class="sm sec" onclick="removeRoomRow(\''+esc(n)+'\')">삭제</button></td></tr>'}).join(''):'<tr><td colspan="10" class="muted">회의실이 없습니다. 아래에서 추가하세요</td></tr>';
  $('cfgRooms').querySelectorAll('[data-k]').forEach(el=>el.onchange=()=>{const i=cfgEdit.rooms[el.dataset.n];if(!i)return;const k=el.dataset.k;
    i[k]=el.type==='checkbox'?el.checked:el.type==='number'?Math.max(0,+el.value||0):el.value})}
function addFloor(){const v=$('newFloor').value.trim();if(!v)return;if(cfgEdit.floors.includes(v)){alert('이미 있는 층입니다');return}cfgEdit.floors.push(v);$('newFloor').value='';renderSettings()}
function removeFloor(i){const f=cfgEdit.floors[i];if(!confirm("'"+f+"' 층을 삭제할까요? 이 층에 속한 회의실은 '미지정'이 됩니다"))return;
  cfgEdit.floors.splice(i,1);Object.values(cfgEdit.rooms).forEach(r=>{if(r.floor===f)r.floor=''});renderSettings()}
function addRoomRow(){const v=$('newRoom').value.trim();if(!v)return;if(cfgEdit.rooms[v]){alert('이미 있는 회의실입니다');return}
  cfgEdit.rooms[v]={floor:'',tv:false,chairs:0,tables:0,whiteboard:false,vc:false,note:''};$('newRoom').value='';renderSettings()}
function removeRoomRow(n){if(!confirm("'"+n+"' 의 기본 정보를 삭제할까요? (카메라/예약이 있으면 이름은 계속 표시됩니다)"))return;delete cfgEdit.rooms[n];renderSettings()}
async function saveConfig(){const m=$('cfgMsg');m.className='msg';m.textContent='저장 중...';
  try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfgEdit)});const j=await r.json();
    if(!j.ok){m.className='msg bad';m.textContent=j.error||'저장 실패';return}
    m.className='msg ok';m.textContent='저장했습니다';await loadConfig();cfgEdit=null;renderSettings()}
  catch(e){m.className='msg bad';m.textContent='요청 실패: '+e}}
// 목록의 작은 아이콘 버튼 (링크 / 수정 / 삭제 - 모두 같은 크기)
const SVG_LINK='<svg viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7"/><path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7"/></svg>';
const SVG_EDIT='<svg viewBox="0 0 24 24"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/></svg>';
const SVG_DEL='<svg viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/><path d="M10 11v6M14 11v6"/></svg>';
const ICON_EDIT='<button class="ib edit" title="수정" onclick="openEdit(\'%ID%\')">'+SVG_EDIT+'</button>';
const ICON_DEL='<button class="ib del" title="예약 취소" onclick="delRes(\'%ID%\')">'+SVG_DEL+'</button>';
// 링크 아이콘: 링크가 있으면 활성(클릭 시 새 탭으로 열림), 없으면 회색
function linkIcon(url){if(!url)return '<span class="lnk off ib" title="링크 없음">'+SVG_LINK+'</span>';const u=esc(url);return '<a class="lnk on ib" href="'+u+'" target="_blank" rel="noopener" title="'+u+'" onclick="event.stopPropagation()">'+SVG_LINK+'</a>'}

// ---- 대시보드: 통계 카드 + 차트 + 로그 ----
let logRows=[];
function setDashSub(k){document.querySelectorAll('.subtab').forEach(e=>e.classList.toggle('active',e.dataset.sub===k));$('dashStats').hidden=k!=='stats';$('dashLog').hidden=k!=='log'}
const fmtMin=m=>m>=60?(Math.floor(m/60)+'시간'+(m%60?' '+m%60+'분':'')):m+'분';
async function loadStats(){const p=$('statPeriod').value;let st;try{st=await(await fetch('/api/stats?period='+p,{cache:'no-store'})).json()}catch(e){return}
  const pn={all:'전체 기간',30:'최근 30일',90:'최근 90일',year:'올해'}[p];$('statNote').textContent='· '+pn+' 기준';
  $('tiles').innerHTML=[
   ['hero','사용 중 회의실',st.occupied_rooms+'<span style="font-size:16px;color:var(--muted)"> / '+st.camera_rooms+'</span>','카메라 기준 현재 재실'],
   ['','오늘 회의',st.today+'건',''],['','이번 달 회의',st.this_month+'건',''],
   ['','누적 회의 건수',st.total+'건',pn],['','평균 회의 시간',fmtMin(st.avg_minutes),pn],['','총 회의 시간',fmtMin(st.total_minutes),pn]
  ].map(([c,l,v,sub])=>'<div class="tile '+c+'"><div class="lbl">'+l+'</div><div class="val">'+v+'</div>'+(sub?'<div class="sub">'+sub+'</div>':'')+'</div>').join('');
  barChart($('chDow'),['월','화','수','목','금','토','일'],st.by_dow,'건');
  barChart($('chHour'),st.hours.map(h=>h+'시'),st.by_hour,'건');
  barChart($('chMonth'),st.by_month.map(x=>(+x.month.slice(5))+'월'),st.by_month.map(x=>x.count),'건',st.by_month.map(x=>x.month.replace('-','년 ')+'월'));
  hbarChart($('chRoom'),st.by_room.map(x=>x.room),st.by_room.map(x=>x.count),'건')}
// 단일 계열 세로 막대 차트 (SVG): 막대 <=24px, 위쪽 4px 라운드, 하이라인 격자, 최대값만 직접 라벨, 마우스 오버 툴팁, 표 보기
function barChart(box,labels,values,unit,fullLabels){   // fullLabels: 툴팁/표에 쓸 긴 이름 (선택)const W=Math.max(300,box.clientWidth||460),H=190,pl=34,pr=8,pt=14,pb=26;const n=labels.length;
  if(!n){box.innerHTML='<div class="muted" style="padding:40px 0;text-align:center">데이터 없음</div>';return}
  const max=Math.max(1,...values);const step=niceStep(max);const top=Math.ceil(max/step)*step;
  const iw=W-pl-pr,ih=H-pt-pb,band=iw/n,bw=Math.min(24,band*0.6);const y=v=>pt+ih-(v/top)*ih;
  let g='';for(let v=0;v<=top;v+=step){g+='<line class="grid" x1="'+pl+'" x2="'+(W-pr)+'" y1="'+y(v)+'" y2="'+y(v)+'"/><text class="axis" x="'+(pl-6)+'" y="'+(y(v)+4)+'" text-anchor="end">'+v+'</text>'}
  const maxI=values.indexOf(max);let bars='';
  values.forEach((v,i)=>{const x=pl+band*i+(band-bw)/2,yy=y(v),h=pt+ih-yy;const r=Math.min(4,h);
    const d=h<=0?'':'M'+x+' '+(pt+ih)+' V'+(yy+r)+' a'+r+' '+r+' 0 0 1 '+r+' -'+r+' H'+(x+bw-r)+' a'+r+' '+r+' 0 0 1 '+r+' '+r+' V'+(pt+ih)+' Z';
    bars+='<rect x="'+(pl+band*i)+'" y="'+pt+'" width="'+band+'" height="'+ih+'" fill="transparent" data-i="'+i+'"/>'+(d?'<path class="bar" d="'+d+'" data-i="'+i+'"/>':'');
    if(i===maxI&&v>0)bars+='<text class="vl" x="'+(x+bw/2)+'" y="'+(yy-4)+'" text-anchor="middle">'+v+'</text>';
    bars+='<text class="axis" x="'+(pl+band*i+band/2)+'" y="'+(H-pb+14)+'" text-anchor="middle">'+esc(String(labels[i]))+'</text>'});
  box.innerHTML='<svg viewBox="0 0 '+W+' '+H+'">'+g+'<line class="grid" x1="'+pl+'" x2="'+(W-pr)+'" y1="'+(pt+ih)+'" y2="'+(pt+ih)+'" style="stroke:#c9ccd1"/>'+bars+'</svg>'
   +'<details><summary>표로 보기</summary><table><thead><tr><th>구분</th><th>값</th></tr></thead><tbody>'+labels.map((l,i)=>'<tr><td>'+esc((fullLabels||labels)[i])+'</td><td>'+values[i]+unit+'</td></tr>').join('')+'</tbody></table></details>';
  const svg=box.querySelector('svg');svg.addEventListener('mousemove',e=>{const t=e.target.closest('[data-i]');if(!t){hideVizTip();return}const i=+t.dataset.i;showVizTip(e.clientX,e.clientY,(fullLabels||labels)[i]+': '+values[i]+unit)});
  svg.addEventListener('mouseleave',hideVizTip)}
// 가로 막대 차트 (긴 이름용): 왼쪽에 이름, 막대 끝에 값
function hbarChart(box,labels,values,unit){const n=labels.length;if(!n){box.innerHTML='<div class="muted" style="padding:40px 0;text-align:center">데이터 없음</div>';return}
  const W=Math.max(300,box.clientWidth||460),rowH=26,pl=110,pr=40,pt=6,H=pt+rowH*n+6;const max=Math.max(1,...values);const iw=W-pl-pr;const bh=18;
  let bars='';values.forEach((v,i)=>{const y=pt+rowH*i+(rowH-bh)/2,w=Math.max(0,v/max*iw),r=Math.min(4,w);
    const d=w<=0?'':'M'+pl+' '+y+' H'+(pl+w-r)+' a'+r+' '+r+' 0 0 1 '+r+' '+r+' V'+(y+bh-r)+' a'+r+' '+r+' 0 0 1 -'+r+' '+r+' H'+pl+' Z';
    const lab=String(labels[i]);bars+='<rect x="0" y="'+(pt+rowH*i)+'" width="'+W+'" height="'+rowH+'" fill="transparent" data-i="'+i+'"/>'+(d?'<path class="bar" d="'+d+'" data-i="'+i+'"/>':'')
     +'<text class="axis" x="'+(pl-8)+'" y="'+(y+bh/2+4)+'" text-anchor="end">'+esc(lab.length>9?lab.slice(0,9)+'…':lab)+'</text>'
     +'<text class="vl" x="'+(pl+w+6)+'" y="'+(y+bh/2+4)+'">'+v+'</text>'});
  box.innerHTML='<svg viewBox="0 0 '+W+' '+H+'" style="height:'+H+'px">'+'<line class="grid" x1="'+pl+'" x2="'+pl+'" y1="'+pt+'" y2="'+(H-6)+'" style="stroke:#c9ccd1"/>'+bars+'</svg>'
   +'<details><summary>표로 보기</summary><table><thead><tr><th>회의실</th><th>값</th></tr></thead><tbody>'+labels.map((l,i)=>'<tr><td>'+esc(l)+'</td><td>'+values[i]+unit+'</td></tr>').join('')+'</tbody></table></details>';
  const svg=box.querySelector('svg');svg.addEventListener('mousemove',e=>{const t=e.target.closest('[data-i]');if(!t){hideVizTip();return}const i=+t.dataset.i;showVizTip(e.clientX,e.clientY,labels[i]+': '+values[i]+unit)});svg.addEventListener('mouseleave',hideVizTip)}
function niceStep(max){const raw=max/4;const p=Math.pow(10,Math.floor(Math.log10(raw)));const f=raw/p;return (f<=1?1:f<=2?2:f<=5?5:10)*p}
function showVizTip(x,y,text){let t=$('vizTip');if(!t){t=document.createElement('div');t.id='vizTip';t.className='viz-tip';document.body.appendChild(t)}t.textContent=text;t.style.left=(x+12)+'px';t.style.top=(y-30)+'px';t.style.display=''}
function hideVizTip(){const t=$('vizTip');if(t)t.style.display='none'}
async function loadLog(){try{logRows=(await(await fetch('/api/log?limit=500',{cache:'no-store'})).json()).log}catch(e){logRows=[]}renderLog()}
function renderLog(){const f=$('logFilter').value;const rows=logRows.filter(r=>!f||r.action===f);
  $('logBody').innerHTML=rows.length?rows.map(r=>{const c=r.action==='예약'?'c':r.action==='수정'?'u':'d';
    return '<tr><td style="white-space:nowrap">'+esc(r.ts)+'</td><td><span class="act '+c+'">'+esc(r.action)+'</span></td><td>'+esc(r.room)+'</td><td style="white-space:nowrap">'+esc(r.date)+' '+esc(r.start)+'~'+esc(r.end)+'</td><td>'+esc(r.reserver)+'</td><td>'+esc(r.purpose)+'</td><td class="muted">'+esc(r.detail)+'</td><td class="muted">'+esc(r.ip)+'</td></tr>'}).join('')
   :'<tr><td colspan="8" class="muted">기록 없음</td></tr>'}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function tick(){const n=new Date();$('clock').textContent=n.toLocaleString('ko-KR');const c=$('tabClock');if(!c)return;
  const dn=['일','월','화','수','목','금','토'][n.getDay()];const hh=String(n.getHours()).padStart(2,'0'),mm=String(n.getMinutes()).padStart(2,'0'),ss=String(n.getSeconds()).padStart(2,'0');
  const tEl=c.querySelector('.time');
  if(tEl&&c.dataset.d===ymd(n)){tEl.textContent=hh+':'+mm+':'+ss;return}   // 날짜가 같으면 시간 글자만 갱신
  c.dataset.d=ymd(n);
  c.innerHTML='<span class="date">'+ymd(n).replace(/-/g,'.')+'.<span class="dow'+(n.getDay()===0?' sun':'')+'">'+dn+'</span></span><span class="sep">|</span><span class="time">'+hh+':'+mm+':'+ss+'</span>'}
setInterval(tick,1000);
loadConfig().then(loadRooms);setInterval(()=>{if(currentTab!=='settings'&&currentTab!=='dash')loadRooms()},4000);setInterval(render,60000);
</script></body></html>"""


ADMIN_HTML = r"""<!DOCTYPE html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>관리자 - 회의실 카메라</title><style>""" + BASE_CSS + r"""
.code{font-family:ui-monospace,Menlo,monospace;font-size:18px;letter-spacing:2px;font-weight:700}
form.inline{display:inline}td form.row{gap:6px}
</style></head><body>
<header><h1>클라이언트 관리</h1><span class="muted">{{ server_time }}</span><span class="sp"></span><a href="/">대시보드</a></header>
<main>
<div class="card"><h2>등록된 클라이언트 <span class="muted">({{ rows|length }}대 · {{ csv_path }})</span></h2>
<p class="muted">기기(ESP32-S3-CAM)가 서버에 접속하면 자동으로 여기에 나타납니다. 기기 웹페이지( http://기기IP/ )의 [서버 인증] 란에 아래 인증코드를 입력하고 [인증 시도]를 누르면 인증됩니다.</p>
{% if not rows %}<div class="muted">아직 접속한 클라이언트가 없습니다. 기기에서 WiFi/서버 IP 설정을 완료하면 자동으로 등록됩니다.</div>{% else %}
<div style="overflow-x:auto"><table>
<thead><tr><th>MAC</th><th>IP</th><th>회의실명</th><th>인증코드</th><th>상태</th><th>재실</th><th>처음 접속</th><th>마지막 접속</th><th>작업</th></tr></thead>
<tbody>
{% for r in rows %}
<tr>
 <td><code>{{ r.mac }}</code></td>
 <td>{% if r.ip %}<a href="{{ r.device_url }}" target="_blank">{{ r.ip }}</a>{% endif %}</td>
 <td><form method="post" action="/admin/action" class="row"><input type="hidden" name="mac" value="{{ r.mac }}"><input type="hidden" name="action" value="room">
     <input type="text" name="room" value="{{ r.room }}" placeholder="회의실명" style="width:150px"><button class="sm sec" type="submit">확인/저장</button></form></td>
 <td>{% if r.authorized %}<span class="muted">-</span>{% else %}<span class="code">{{ r.auth_code }}</span>{% endif %}</td>
 <td>{% if r.authorized %}<span class="badge ok">인증됨</span>{% else %}<span class="badge warn">인증 대기</span>{% endif %}
     {% if r.online %}<span class="badge acc">온라인</span>{% else %}<span class="badge">오프라인</span>{% endif %}</td>
 <td>{% if r.online %}{% if r.occupied %}<span class="badge ok">재실 ({{ r.faces }})</span>{% else %}<span class="muted">비어있음</span>{% endif %}{% else %}<span class="muted">-</span>{% endif %}</td>
 <td class="muted">{{ r.first_seen }}</td>
 <td class="muted">{{ r.last_seen }}</td>
 <td><div class="row">
  {% if r.authorized %}<form method="post" action="/admin/action" class="inline"><input type="hidden" name="mac" value="{{ r.mac }}"><input type="hidden" name="action" value="revoke"><button class="sm bad" onclick="return confirm('인증을 취소하고 새 인증코드를 발급합니다. 계속할까요?')">인증 취소</button></form>
  {% else %}<form method="post" action="/admin/action" class="inline"><input type="hidden" name="mac" value="{{ r.mac }}"><input type="hidden" name="action" value="regen"><button class="sm sec">코드 재발급</button></form>{% endif %}
  {% if r.stream_url %}<a href="{{ r.stream_url }}" target="_blank"><button class="sm sec" type="button">영상</button></a>{% endif %}
  <form method="post" action="/admin/action" class="inline"><input type="hidden" name="mac" value="{{ r.mac }}"><input type="hidden" name="action" value="delete"><button class="sm sec" onclick="return confirm('이 클라이언트 기록을 삭제할까요?')">삭제</button></form>
 </div></td>
</tr>
{% endfor %}
</tbody></table></div>{% endif %}
</div>
</main>
<script>setTimeout(()=>location.reload(),15000)</script>
</body></html>"""


# ---------------------------------------------------------------------------
if __name__ == "__main__":
    load_clients()
    os.makedirs(SNAPSHOT_DIR, exist_ok=True)
    print("회의실 예약 서버 시작: http://%s:%d/  (관리자: /admin)" % (HOST, PORT))
    print("clients.csv: %s (%d대 등록)" % (CLIENTS_CSV, len(clients)))
    print("reservations.csv: %s" % RESERVATIONS_CSV)
    init_face_detector()
    init_person_detector()
    print("서버측 검출/블러: " + detector_status())
    if not ADMIN_PASSWORD:
        print("주의: ADMIN_PASSWORD 가 설정되지 않아 /admin 에 누구나 접근할 수 있습니다.")
    app.run(host=HOST, port=PORT, threaded=True, debug=False)
