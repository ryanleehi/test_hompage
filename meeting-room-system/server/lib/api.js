'use strict';
const cfg = require('./config');
const U = require('./util');
const { roomView, runMaintenance, findConflict } = require('./logic');

function deviceAuthOk(req) {
  return (req.headers['x-device-token'] || '') === cfg.DEVICE_TOKEN;
}

function adminOk(req) {
  if (!cfg.ADMIN_TOKEN) return true;              // 토큰을 설정하지 않았으면 제한 없음
  return (req.headers['x-admin-token'] || '') === cfg.ADMIN_TOKEN;
}

function normalizeReservationInput(p) {
  const roomId = U.clean(p.roomId, 40);
  const date = U.clean(p.date, 10) || U.todayString();
  const start = U.toMinutes(p.start);
  const end = U.toMinutes(p.end);
  const title = U.clean(p.title, 40) || '회의';
  const user = U.clean(p.user, 24) || '익명';
  const contact = U.clean(p.contact, 40);
  return { roomId, date, start, end, title, user, contact };
}

function validateReservation(store, input, opts = {}) {
  const { roomId, date, start, end } = input;
  if (!roomId || !store.room(roomId)) return '등록되지 않은 회의실입니다.';
  if (!U.isValidDate(date)) return '날짜 형식이 올바르지 않습니다. (YYYY-MM-DD)';
  if (!Number.isFinite(start) || !Number.isFinite(end)) return '시작/종료 시각이 올바르지 않습니다.';
  if (end <= start) return '종료 시각은 시작 시각보다 늦어야 합니다.';
  if (end > 1440) return '종료 시각이 24:00 을 넘을 수 없습니다.';
  if (end - start > 8 * 60) return '한 번에 8시간을 넘겨 예약할 수 없습니다.';

  if (!opts.skipHours) {
    if (start < cfg.OPEN_MINUTE || end > cfg.CLOSE_MINUTE) {
      return `예약 가능 시간은 ${U.toHm(cfg.OPEN_MINUTE)} ~ ${U.toHm(cfg.CLOSE_MINUTE)} 입니다.`;
    }
  }

  const today = U.todayString();
  if (date < today) return '지난 날짜에는 예약할 수 없습니다.';
  if (date === today && end <= U.nowMinutes()) return '이미 지난 시간입니다.';

  const conflict = findConflict(store, roomId, date, start, end);
  if (conflict) {
    return `이미 ${U.toHm(conflict.start)}~${U.toHm(conflict.end)} '${conflict.title}' 예약이 있습니다.`;
  }
  return null;
}

function createReservation(store, input, source) {
  const res = {
    id: U.makeId('R'),
    roomId: input.roomId,
    date: input.date,
    start: input.start,
    end: input.end,
    title: input.title,
    user: input.user,
    contact: input.contact || '',
    status: 'booked',
    source,
    checkedIn: false,
    checkedInAt: null,
    createdAt: new Date().toISOString(),
  };
  store.addReservation(res);
  const room = store.room(input.roomId);
  if (room) room.pullFlag = true;                 // 기기가 다음 하트비트에 내려받도록
  store.logEvent(input.roomId, 'book', `${res.user} 님이 ${U.toHm(res.start)}~${U.toHm(res.end)} '${res.title}' 예약`);
  return res;
}

/**
 * @returns {boolean} 이 요청을 처리했으면 true
 */
async function handleApi(req, res, url, store) {
  const p = url.pathname;
  const method = req.method.toUpperCase();

  if (!p.startsWith('/api/')) return false;

  if (method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET,POST,DELETE,OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type,X-Device-Token,X-Admin-Token',
    });
    res.end();
    return true;
  }

  // =========================================================================
  // 기기(ESP32) 전용 API - 파이프 구분 텍스트
  // =========================================================================
  if (p.startsWith('/api/devices/')) {
    if (!deviceAuthOk(req)) {
      U.sendText(res, 401, 'ERR|AUTH');
      return true;
    }

    if (p === '/api/devices/register' && method === 'POST') {
      const b = await U.readParams(req);
      const roomId = U.clean(b.roomId, 40);
      if (!roomId) { U.sendText(res, 400, 'ERR|ROOMID'); return true; }
      const existed = !!store.room(roomId);
      store.upsertRoom(roomId, {
        name: U.clean(b.roomName, 40) || roomId,
        ip: U.clean(b.ip, 45),
        mac: U.clean(b.mac, 20),
        fw: U.clean(b.fw, 16),
        streamPort: parseInt(b.streamPort, 10) || 81,
        hasCamera: b.hasCamera !== '0',
        pullFlag: true,
      });
      if (!existed) store.logEvent(roomId, 'register', `새 기기가 등록되었습니다. (${U.clean(b.ip, 45)})`);
      U.sendText(res, 200, 'OK');
      return true;
    }

    if (p === '/api/devices/heartbeat' && method === 'POST') {
      const b = await U.readParams(req);
      const roomId = U.clean(b.roomId, 40);
      const room = store.room(roomId);
      if (!room) { U.sendText(res, 404, 'ERR|UNREGISTERED'); return true; }

      const wasOccupied = !!room.occupied;
      const occupied = b.occupied === '1' || b.occupied === 'true';
      store.upsertRoom(roomId, {
        name: U.clean(b.roomName, 40) || room.name,
        occupied,
        lastMotion: parseInt(b.lastMotion, 10),
        motionCount: parseInt(b.motionCount, 10) || 0,
        rssi: parseInt(b.rssi, 10),
        ip: U.clean(b.ip, 45) || room.ip,
        uptime: parseInt(b.uptime, 10) || 0,
        heap: parseInt(b.heap, 10) || 0,
        fw: U.clean(b.fw, 16) || room.fw,
        streamPort: parseInt(b.streamPort, 10) || room.streamPort || 81,
      });
      if (wasOccupied !== occupied) {
        store.logEvent(roomId, occupied ? 'occupied' : 'vacated', occupied ? '사람이 감지되었습니다.' : '비어 있는 상태로 바뀌었습니다.');
      }

      const fresh = store.room(roomId);
      const needPull = !!fresh.pullFlag;
      if (needPull) { fresh.pullFlag = false; store.save(); }
      U.sendText(res, 200, needPull ? 'OK\nPULL|1\n' : 'OK\n');
      return true;
    }

    if (p === '/api/devices/reservations' && method === 'GET') {
      const roomId = U.clean(url.searchParams.get('roomId'), 40);
      const date = U.clean(url.searchParams.get('date'), 10) || U.todayString();
      if (!store.room(roomId)) { U.sendText(res, 404, 'ERR|UNREGISTERED'); return true; }
      runMaintenance(store);

      const list = store
        .reservations({ roomId, date, activeOnly: true })
        .filter((r) => !(cfg.AUTO_RELEASE && r.status === 'no_show'))
        .sort((a, b) => a.start - b.start);

      let out = `DATE|${date}\nNOW|${U.nowMinutes()}\n`;
      for (const r of list) {
        out += `RES|${r.id}|${r.start}|${r.end}|${U.clean(r.title, 40)}|${U.clean(r.user, 24)}\n`;
      }
      U.sendText(res, 200, out);
      return true;
    }

    if (p === '/api/devices/reservations' && method === 'POST') {
      const b = await U.readParams(req);
      const input = normalizeReservationInput(b);
      // 기기에서 올라온 "지금 바로 사용"은 운영시간 제한을 적용하지 않는다.
      const err = validateReservation(store, input, { skipHours: true });
      if (err) { U.sendText(res, 409, 'ERR|' + err); return true; }
      const created = createReservation(store, input, 'device');
      U.sendText(res, 200, `OK|${created.id}`);
      return true;
    }

    if (p === '/api/devices/cancel' && method === 'POST') {
      const b = await U.readParams(req);
      const r = store.reservation(U.clean(b.id, 40));
      if (!r) { U.sendText(res, 404, 'ERR|NOTFOUND'); return true; }
      store.updateReservation(r.id, { status: 'canceled' });
      store.logEvent(r.roomId, 'cancel', `기기에서 '${r.title}' 예약이 취소되었습니다.`);
      U.sendText(res, 200, 'OK');
      return true;
    }

    if (p === '/api/devices/snapshot' && method === 'POST') {
      const roomId = U.clean(url.searchParams.get('roomId'), 40);
      if (!store.room(roomId)) { U.sendText(res, 404, 'ERR|UNREGISTERED'); return true; }
      const buf = await U.readBody(req, 1024 * 1024);
      if (!buf.length) { U.sendText(res, 400, 'ERR|EMPTY'); return true; }
      store.writeSnapshot(roomId, buf);
      U.sendText(res, 200, 'OK');
      return true;
    }

    U.sendText(res, 404, 'ERR|NOROUTE');
    return true;
  }

  // =========================================================================
  // 브라우저용 JSON API
  // =========================================================================
  if (p === '/api/config' && method === 'GET') {
    U.sendJson(res, 200, {
      openMinute: cfg.OPEN_MINUTE,
      closeMinute: cfg.CLOSE_MINUTE,
      noShowMinutes: cfg.NO_SHOW_MINUTES,
      autoRelease: cfg.AUTO_RELEASE,
      offlineAfterSec: cfg.OFFLINE_AFTER_SEC,
      requiresAdminToken: !!cfg.ADMIN_TOKEN,
    });
    return true;
  }

  if (p === '/api/rooms' && method === 'GET') {
    runMaintenance(store);
    const date = U.clean(url.searchParams.get('date'), 10) || U.todayString();
    const rooms = store.rooms().map((r) => roomView(store, r, date));
    rooms.sort((a, b) => a.name.localeCompare(b.name, 'ko'));
    U.sendJson(res, 200, {
      serverTime: new Date().toISOString(),
      date,
      nowMin: U.nowMinutes(),
      summary: {
        total: rooms.length,
        online: rooms.filter((r) => r.online).length,
        inUse: rooms.filter((r) => r.state === 'in_use' || r.state === 'occupied').length,
        free: rooms.filter((r) => r.state === 'free').length,
        alerts: rooms.filter((r) => r.unbookedUse || r.ghostReservation).length,
      },
      rooms,
    });
    return true;
  }

  let m = p.match(/^\/api\/rooms\/([^/]+)$/);
  if (m && method === 'GET') {
    const room = store.room(decodeURIComponent(m[1]));
    if (!room) { U.sendJson(res, 404, { ok: false, error: '회의실을 찾을 수 없습니다.' }); return true; }
    runMaintenance(store);
    const date = U.clean(url.searchParams.get('date'), 10) || U.todayString();
    U.sendJson(res, 200, roomView(store, room, date));
    return true;
  }

  if (m && method === 'DELETE') {
    if (!adminOk(req)) { U.sendJson(res, 403, { ok: false, error: '관리자 토큰이 필요합니다.' }); return true; }
    const id = decodeURIComponent(m[1]);
    const ok = store.removeRoom(id);
    U.sendJson(res, ok ? 200 : 404, { ok });
    return true;
  }

  m = p.match(/^\/api\/rooms\/([^/]+)\/snapshot$/);
  if (m && method === 'GET') {
    const buf = store.readSnapshot(decodeURIComponent(m[1]));
    if (!buf) { U.sendJson(res, 404, { ok: false, error: '스냅샷이 아직 없습니다.' }); return true; }
    res.writeHead(200, {
      'Content-Type': 'image/jpeg',
      'Content-Length': buf.length,
      'Cache-Control': 'no-store',
      'Access-Control-Allow-Origin': '*',
    });
    res.end(buf);
    return true;
  }

  if (p === '/api/reservations' && method === 'GET') {
    const date = U.clean(url.searchParams.get('date'), 10) || U.todayString();
    const roomId = U.clean(url.searchParams.get('roomId'), 40) || null;
    const list = store
      .reservations({ date, roomId: roomId || undefined, activeOnly: true })
      .sort((a, b) => a.start - b.start || a.roomId.localeCompare(b.roomId));
    U.sendJson(res, 200, { date, reservations: list });
    return true;
  }

  if (p === '/api/reservations' && method === 'POST') {
    const b = await U.readParams(req);
    const input = normalizeReservationInput(b);
    const err = validateReservation(store, input);
    if (err) { U.sendJson(res, 409, { ok: false, error: err }); return true; }
    const created = createReservation(store, input, 'web');
    U.sendJson(res, 201, { ok: true, reservation: created });
    return true;
  }

  m = p.match(/^\/api\/reservations\/([^/]+)\/cancel$/);
  if (m && method === 'POST') {
    const b = await U.readParams(req);
    const r = store.reservation(decodeURIComponent(m[1]));
    if (!r) { U.sendJson(res, 404, { ok: false, error: '예약을 찾을 수 없습니다.' }); return true; }
    if (r.status === 'canceled') { U.sendJson(res, 200, { ok: true }); return true; }

    const claimed = U.clean(b.user, 24).toLowerCase();
    const owner = U.clean(r.user, 24).toLowerCase();
    const isAdmin = cfg.ADMIN_TOKEN && (req.headers['x-admin-token'] || '') === cfg.ADMIN_TOKEN;
    if (!isAdmin && owner && claimed !== owner) {
      U.sendJson(res, 403, { ok: false, error: '예약자 이름이 일치해야 취소할 수 있습니다.' });
      return true;
    }

    store.updateReservation(r.id, { status: 'canceled' });
    const room = store.room(r.roomId);
    if (room) room.pullFlag = true;
    store.logEvent(r.roomId, 'cancel', `'${r.title}' 예약이 취소되었습니다.`);
    U.sendJson(res, 200, { ok: true });
    return true;
  }

  if (p === '/api/events' && method === 'GET') {
    const limit = Math.min(200, parseInt(url.searchParams.get('limit') || '40', 10) || 40);
    U.sendJson(res, 200, { events: store.events(limit) });
    return true;
  }

  U.sendJson(res, 404, { ok: false, error: '존재하지 않는 API 경로입니다.' });
  return true;
}

module.exports = { handleApi };
