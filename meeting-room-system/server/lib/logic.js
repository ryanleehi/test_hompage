'use strict';
const cfg = require('./config');
const { todayString, nowMinutes, agoSeconds, overlaps } = require('./util');

/**
 * 회의실 하나의 "지금 상태"를 계산한다.
 * 기기가 보고한 재실 여부 + 예약 정보를 합쳐 대시보드가 바로 쓸 수 있는 형태로 만든다.
 */
function roomView(store, room, date = todayString(), now = nowMinutes()) {
  const online = room.lastSeen ? agoSeconds(room.lastSeen) <= cfg.OFFLINE_AFTER_SEC : false;
  const occupied = online ? !!room.occupied : false;

  const list = store
    .reservations({ roomId: room.id, date, activeOnly: true })
    .filter((r) => r.status !== 'no_show' || !cfg.AUTO_RELEASE)
    .sort((a, b) => a.start - b.start);

  const current = list.find((r) => now >= r.start && now < r.end) || null;
  const next = list.find((r) => r.start > now) || null;

  let state = 'free';           // free | occupied | reserved | in_use | unreported
  if (!online) state = 'unreported';
  else if (current && occupied) state = 'in_use';
  else if (current && !occupied) state = 'reserved';
  else if (occupied) state = 'occupied';

  return {
    id: room.id,
    name: room.name || room.id,
    online,
    occupied,
    state,
    ip: room.ip || null,
    streamPort: room.streamPort || 81,
    streamUrl: room.ip ? `http://${room.ip}:${room.streamPort || 81}/stream` : null,
    hasCamera: room.hasCamera !== false,
    rssi: room.rssi ?? null,
    fw: room.fw || null,
    uptime: room.uptime ?? null,
    lastSeenAgo: agoSeconds(room.lastSeen),
    lastMotion: room.lastMotion ?? -1,
    motionCount: room.motionCount ?? 0,
    snapshotAgo: agoSeconds(room.snapshotAt),
    hasSnapshot: !!room.snapshotAt,
    date,
    nowMin: now,
    current,
    next,
    reservations: list,
    // 예약 없이 사용 중 / 예약했는데 아무도 없음 → 운영자가 보고 싶어 하는 두 가지 이상 신호
    unbookedUse: online && occupied && !current,
    ghostReservation: online && !!current && !occupied && now - current.start >= cfg.NO_SHOW_MINUTES,
  };
}

/**
 * 주기적으로 도는 정리 작업.
 *  - 예약 시간에 사람이 감지되면 자동 체크인
 *  - 시작 후 NO_SHOW_MINUTES 동안 아무도 없으면 노쇼 처리 (AUTO_RELEASE 면 자리도 비움)
 */
function runMaintenance(store) {
  const date = todayString();
  const now = nowMinutes();

  for (const room of store.rooms()) {
    const online = room.lastSeen ? agoSeconds(room.lastSeen) <= cfg.OFFLINE_AFTER_SEC : false;
    const list = store.reservations({ roomId: room.id, date, activeOnly: true });

    for (const r of list) {
      if (now < r.start || now >= r.end) continue;
      if (!online) continue;

      if (room.occupied && !r.checkedIn) {
        store.updateReservation(r.id, {
          checkedIn: true,
          checkedInAt: new Date().toISOString(),
          status: 'booked',
        });
        store.logEvent(room.id, 'check_in', `'${r.title}' 사용 시작이 감지되었습니다.`);
      } else if (!room.occupied && !r.checkedIn && now - r.start >= cfg.NO_SHOW_MINUTES && r.status !== 'no_show') {
        store.updateReservation(r.id, { status: 'no_show' });
        store.logEvent(
          room.id,
          'no_show',
          `'${r.title}' 예약이 시작 후 ${cfg.NO_SHOW_MINUTES}분간 사용되지 않아 ` +
            (cfg.AUTO_RELEASE ? '자동으로 해제되었습니다.' : '노쇼로 표시되었습니다.')
        );
        if (room) room.pullFlag = true;
      }
    }
  }
  store.save();
}

/** 예약 시간이 겹치는지 검사 */
function findConflict(store, roomId, date, start, end, excludeId = null) {
  return (
    store
      .reservations({ roomId, date, activeOnly: true })
      .filter((r) => r.id !== excludeId)
      .filter((r) => !(cfg.AUTO_RELEASE && r.status === 'no_show'))
      .find((r) => overlaps(start, end, r.start, r.end)) || null
  );
}

module.exports = { roomView, runMaintenance, findConflict };
