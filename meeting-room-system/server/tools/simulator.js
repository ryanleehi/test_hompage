#!/usr/bin/env node
'use strict';
/**
 * 기기 시뮬레이터 - ESP32 하드웨어 없이 서버/대시보드를 시험할 때 사용한다.
 *
 *   node tools/simulator.js --rooms 4 --server http://localhost:3000 --token change-me-token
 *
 * 등록 → 하트비트(재실 상태 랜덤 변화) → 예약 목록 내려받기 → 스냅샷 업로드를
 * 실제 펌웨어와 같은 프로토콜로 수행한다.
 */
const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, cur, i, arr) => {
    if (cur.startsWith('--')) acc.push([cur.slice(2), arr[i + 1] && !arr[i + 1].startsWith('--') ? arr[i + 1] : 'true']);
    return acc;
  }, [])
);

const SERVER = (args.server || process.env.SERVER || 'http://localhost:3000').replace(/\/$/, '');
const TOKEN = args.token || process.env.DEVICE_TOKEN || 'change-me-token';
const COUNT = parseInt(args.rooms || '3', 10);

const NAMES = ['3층 대회의실', '5층 소회의실 A', '5층 소회의실 B', '본관 세미나실', '연구동 회의실'];

// 1x1 회색 JPEG (스냅샷 업로드 경로 확인용)
const TINY_JPEG = Buffer.from(
  '/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a' +
  'HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA' +
  'AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==',
  'base64'
);

function form(obj) {
  return new URLSearchParams(obj).toString();
}

async function post(path, body, contentType = 'application/x-www-form-urlencoded') {
  const r = await fetch(SERVER + path, {
    method: 'POST',
    headers: { 'Content-Type': contentType, 'X-Device-Token': TOKEN },
    body,
  });
  return { status: r.status, text: await r.text() };
}

async function get(path) {
  const r = await fetch(SERVER + path, { headers: { 'X-Device-Token': TOKEN } });
  return { status: r.status, text: await r.text() };
}

class FakeRoom {
  constructor(i) {
    this.id = `room-${i + 1}`;
    this.name = NAMES[i % NAMES.length] + (i >= NAMES.length ? ` #${i + 1}` : '');
    this.ip = `192.168.0.${20 + i}`;
    this.occupied = false;
    this.lastMotion = -1;
    this.motionCount = 0;
    this.bootAt = Date.now();
  }

  tick() {
    // 5% 확률로 사람이 들어오고, 사용중이면 3% 확률로 나간다.
    if (!this.occupied && Math.random() < 0.05) this.occupied = true;
    else if (this.occupied && Math.random() < 0.03) this.occupied = false;
    if (this.occupied) { this.lastMotion = Math.floor(Math.random() * 20); this.motionCount++; }
    else if (this.lastMotion >= 0) this.lastMotion += 10;
  }

  async register() {
    const r = await post('/api/devices/register', form({
      roomId: this.id, roomName: this.name, ip: this.ip,
      mac: `AA:BB:CC:00:00:${String(10 + Number(this.id.split('-')[1])).padStart(2, '0')}`,
      fw: '1.0.0-sim', streamPort: 81, hasCamera: 1,
    }));
    console.log(`[${this.id}] 등록 → ${r.status} ${r.text.trim()}`);
  }

  async heartbeat() {
    this.tick();
    const r = await post('/api/devices/heartbeat', form({
      roomId: this.id, roomName: this.name,
      occupied: this.occupied ? 1 : 0,
      lastMotion: this.lastMotion, motionCount: this.motionCount,
      rssi: -40 - Math.floor(Math.random() * 30), ip: this.ip,
      uptime: Math.floor((Date.now() - this.bootAt) / 1000),
      heap: 210000, fw: '1.0.0-sim', streamPort: 81,
    }));
    if (r.text.includes('PULL|1')) await this.pull();
  }

  async pull() {
    const date = new Date().toISOString().slice(0, 10);
    const r = await get(`/api/devices/reservations?roomId=${this.id}&date=${date}`);
    const lines = r.text.split('\n').filter((l) => l.startsWith('RES|'));
    console.log(`[${this.id}] 예약 동기화 → ${lines.length}건`);
  }

  async snapshot() {
    await post(`/api/devices/snapshot?roomId=${this.id}`, TINY_JPEG, 'image/jpeg');
  }
}

(async function main() {
  const rooms = Array.from({ length: COUNT }, (_, i) => new FakeRoom(i));
  console.log(`시뮬레이터 시작: ${SERVER} 에 회의실 ${COUNT}개 등록`);
  for (const r of rooms) { await r.register(); await r.snapshot(); await r.pull(); }

  setInterval(() => rooms.forEach((r) => r.heartbeat().catch((e) => console.error(e.message))), 5000);
  setInterval(() => rooms.forEach((r) => r.snapshot().catch(() => {})), 30000);
  console.log('5초마다 하트비트를 보냅니다. 중지하려면 Ctrl+C.');
})();
