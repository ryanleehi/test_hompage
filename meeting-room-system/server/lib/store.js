'use strict';
const fs = require('fs');
const path = require('path');

/**
 * 아주 단순한 JSON 파일 저장소.
 * 외부 DB 없이 바로 실행되는 것을 우선했고, 쓰기는 임시 파일 + rename 으로 원자적으로 처리한다.
 * 규모가 커지면 이 파일만 SQLite/Postgres 구현으로 교체하면 된다.
 */
class Store {
  constructor(dataDir) {
    this.dataDir = dataDir;
    this.file = path.join(dataDir, 'db.json');
    this.snapshotDir = path.join(dataDir, 'snapshots');
    this.saveTimer = null;
    fs.mkdirSync(this.snapshotDir, { recursive: true });
    this.data = this._load();
  }

  _load() {
    try {
      const raw = fs.readFileSync(this.file, 'utf8');
      const parsed = JSON.parse(raw);
      return {
        rooms: parsed.rooms || {},
        reservations: Array.isArray(parsed.reservations) ? parsed.reservations : [],
        events: Array.isArray(parsed.events) ? parsed.events : [],
      };
    } catch {
      return { rooms: {}, reservations: [], events: [] };
    }
  }

  /** 잦은 쓰기를 모아서 처리 */
  save() {
    if (this.saveTimer) return;
    this.saveTimer = setTimeout(() => {
      this.saveTimer = null;
      this.saveNow();
    }, 300);
  }

  saveNow() {
    const tmp = this.file + '.tmp';
    try {
      fs.writeFileSync(tmp, JSON.stringify(this.data, null, 2));
      fs.renameSync(tmp, this.file);
    } catch (err) {
      console.error('[store] 저장 실패:', err.message);
    }
  }

  // --- 회의실 ---------------------------------------------------------------
  rooms() {
    return Object.values(this.data.rooms);
  }

  room(id) {
    return this.data.rooms[id] || null;
  }

  upsertRoom(id, patch) {
    const now = new Date().toISOString();
    const existing = this.data.rooms[id];
    const room = existing || {
      id,
      name: id,
      firstSeen: now,
      occupied: false,
      lastMotion: -1,
      motionCount: 0,
      pullFlag: false,
      snapshotAt: null,
    };
    Object.assign(room, patch, { lastSeen: now });
    this.data.rooms[id] = room;
    this.save();
    return room;
  }

  removeRoom(id) {
    if (!this.data.rooms[id]) return false;
    delete this.data.rooms[id];
    this.data.reservations = this.data.reservations.filter((r) => r.roomId !== id);
    this.save();
    return true;
  }

  // --- 예약 -----------------------------------------------------------------
  reservations(filter = {}) {
    return this.data.reservations.filter((r) => {
      if (filter.roomId && r.roomId !== filter.roomId) return false;
      if (filter.date && r.date !== filter.date) return false;
      if (filter.activeOnly && r.status === 'canceled') return false;
      return true;
    });
  }

  reservation(id) {
    return this.data.reservations.find((r) => r.id === id) || null;
  }

  addReservation(res) {
    this.data.reservations.push(res);
    this.save();
    return res;
  }

  updateReservation(id, patch) {
    const r = this.reservation(id);
    if (!r) return null;
    Object.assign(r, patch);
    this.save();
    return r;
  }

  /** 오래된 예약 정리 (기본 60일 이전) */
  purgeOld(days = 60) {
    const cutoff = new Date(Date.now() - days * 86400000);
    const cutoffStr = cutoff.toISOString().slice(0, 10);
    const before = this.data.reservations.length;
    this.data.reservations = this.data.reservations.filter((r) => r.date >= cutoffStr);
    if (this.data.reservations.length !== before) this.save();
  }

  // --- 이벤트 로그 ----------------------------------------------------------
  logEvent(roomId, type, message) {
    this.data.events.unshift({ ts: new Date().toISOString(), roomId, type, message });
    if (this.data.events.length > 300) this.data.events.length = 300;
    this.save();
  }

  events(limit = 50) {
    return this.data.events.slice(0, limit);
  }

  // --- 스냅샷 ---------------------------------------------------------------
  snapshotPath(roomId) {
    const safe = String(roomId).replace(/[^a-zA-Z0-9_-]/g, '_');
    return path.join(this.snapshotDir, safe + '.jpg');
  }

  writeSnapshot(roomId, buffer) {
    fs.writeFileSync(this.snapshotPath(roomId), buffer);
    const room = this.data.rooms[roomId];
    if (room) {
      room.snapshotAt = new Date().toISOString();
      this.save();
    }
  }

  readSnapshot(roomId) {
    try {
      return fs.readFileSync(this.snapshotPath(roomId));
    } catch {
      return null;
    }
  }
}

module.exports = { Store };
