'use strict';
/* 회의실 통합 대시보드 클라이언트 (프레임워크 없이 동작) */

const $ = (s) => document.querySelector(s);
const STATE_LABEL = {
  in_use: '사용중',
  occupied: '예약 없이 사용중',
  reserved: '예약됨 (비어있음)',
  free: '비어있음',
  unreported: '기기 오프라인',
};

let CFG = { openMinute: 480, closeMinute: 1320, noShowMinutes: 10 };
let ROOMS = [];
let openRoomId = null;
let selectedDate = todayStr();

// ---------- 유틸 ----------
function pad(n) { return String(n).padStart(2, '0'); }
function hm(m) { return pad(Math.floor(m / 60)) + ':' + pad(m % 60); }
function todayStr(d = new Date()) { return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()); }
function esc(s) { return String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function ago(sec) {
  if (sec == null || sec < 0) return '없음';
  if (sec < 60) return sec + '초 전';
  if (sec < 3600) return Math.floor(sec / 60) + '분 전';
  if (sec < 86400) return Math.floor(sec / 3600) + '시간 전';
  return Math.floor(sec / 86400) + '일 전';
}
async function api(url, opts) {
  const r = await fetch(url, opts);
  const text = await r.text();
  let data = {};
  try { data = JSON.parse(text); } catch { /* 빈 응답 */ }
  if (!r.ok) throw new Error(data.error || '요청에 실패했습니다.');
  return data;
}

// ---------- 시계 ----------
setInterval(() => {
  const d = new Date();
  $('#clock').textContent =
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}, 1000);

// ---------- 타임라인 ----------
function timeline(room) {
  const open = CFG.openMinute, close = CFG.closeMinute, span = close - open;
  const pct = (m) => Math.max(0, Math.min(100, ((m - open) / span) * 100));
  let h = `<div class="tl"><span class="lbl">${hm(open)}</span><span class="lbl r">${hm(close)}</span>`;
  for (const r of room.reservations) {
    const left = pct(r.start), width = Math.max(1.2, pct(r.end) - pct(r.start));
    const live = room.nowMin >= r.start && room.nowMin < r.end;
    const cls = live ? 'live' : room.nowMin >= r.end ? 'done' : '';
    h += `<i class="${cls}" style="left:${left}%;width:${width}%" title="${esc(r.title)} ${hm(r.start)}~${hm(r.end)}"></i>`;
  }
  if (room.date === todayStr() && room.nowMin >= open && room.nowMin <= close) {
    h += `<span class="now" style="left:${pct(room.nowMin)}%"></span>`;
  }
  return h + '</div>';
}

// ---------- 카드 ----------
function roomCard(room) {
  const shot = room.hasSnapshot
    ? `<img class="shot" src="/api/rooms/${encodeURIComponent(room.id)}/snapshot?t=${Date.now()}" alt="${esc(room.name)} 화면" loading="lazy" data-room="${esc(room.id)}">`
    : `<div class="noshot" data-room="${esc(room.id)}">${room.online ? '스냅샷 준비 중…' : '기기 오프라인'}</div>`;

  const cur = room.current;
  const nxt = room.next;
  let alert = '';
  if (room.unbookedUse) alert = '⚠ 예약 없이 사용 중입니다.';
  else if (room.ghostReservation) alert = `⚠ 예약 시간인데 ${CFG.noShowMinutes}분 이상 사람이 없습니다.`;
  else if (!room.online) alert = `⚠ 마지막 통신 ${ago(room.lastSeenAgo)}`;

  return `<article class="room">
    ${shot}
    <div class="body">
      <h3>${esc(room.name)}
        <span class="badge s-${room.state}"><span class="dot"></span>${STATE_LABEL[room.state]}</span>
      </h3>
      <div class="line">${esc(room.id)} · 마지막 움직임 ${ago(room.lastMotion)}</div>
      ${timeline(room)}
      <div class="strong">${cur ? `진행중: ${esc(cur.title)} (${hm(cur.start)}~${hm(cur.end)}, ${esc(cur.user)})` : '현재 예약 없음'}</div>
      <div class="line">${nxt ? `다음: ${hm(nxt.start)} ${esc(nxt.title)}` : '남은 예약 없음'}</div>
      ${alert ? `<div class="alertline">${alert}</div>` : ''}
      <div class="acts">
        <button data-book="${esc(room.id)}">예약하기</button>
        <button class="ghost" data-open="${esc(room.id)}">상세 보기</button>
      </div>
    </div>
  </article>`;
}

// ---------- 목록 갱신 ----------
async function refresh() {
  try {
    const d = await api('/api/rooms?date=' + encodeURIComponent(selectedDate));
    ROOMS = d.rooms;
    $('#conn').className = 'conn on';

    $('#summary').innerHTML = [
      ['전체 회의실', d.summary.total, ''],
      ['사용중', d.summary.inUse, ''],
      ['비어있음', d.summary.free, ''],
      ['온라인 기기', `${d.summary.online}/${d.summary.total}`, ''],
      ['확인 필요', d.summary.alerts, 'alert'],
    ].map(([k, v, c]) => `<div class="chip ${c}"><b>${v}</b><span>${k}</span></div>`).join('');

    $('#grid').innerHTML = ROOMS.length
      ? ROOMS.map(roomCard).join('')
      : `<div class="empty">아직 등록된 회의실이 없습니다.<br><br>
           ESP32 기기의 설정 화면에서 이 서버 주소를 입력하면 자동으로 등록됩니다.</div>`;

    if (openRoomId) renderModal();
  } catch (e) {
    $('#conn').className = 'conn off';
  }
}

async function refreshEvents() {
  try {
    const d = await api('/api/events?limit=25');
    $('#events').innerHTML = d.events.length
      ? d.events.map((e) => {
          const t = new Date(e.ts);
          const room = ROOMS.find((r) => r.id === e.roomId);
          return `<li><time>${pad(t.getHours())}:${pad(t.getMinutes())}</time>
            <span class="who">${esc(room ? room.name : e.roomId)}</span>
            <span>${esc(e.message)}</span></li>`;
        }).join('')
      : '<li class="mut">기록이 없습니다.</li>';
  } catch { /* 무시 */ }
}

// ---------- 모달 ----------
function openModal(id, focusForm) {
  openRoomId = id;
  $('#modalBack').hidden = false;
  renderModal();
  if (focusForm) setTimeout(() => $('#fStart').focus(), 80);
}
function closeModal() {
  openRoomId = null;
  $('#modalBack').hidden = true;
  $('#fMsg').textContent = '';
}

function renderModal() {
  const room = ROOMS.find((r) => r.id === openRoomId);
  if (!room) return closeModal();

  $('#mTitle').textContent = room.name;
  $('#mSub').textContent = `${room.id} · ${selectedDate} · ${room.online ? '기기 온라인' : '기기 오프라인'}`;

  const img = $('#mShot');
  const src = room.hasSnapshot ? `/api/rooms/${encodeURIComponent(room.id)}/snapshot?t=${Date.now()}` : '';
  if (src) img.src = src; else img.removeAttribute('src');

  const live = $('#mLive'), dev = $('#mDevice');
  if (room.ip && room.hasCamera) {
    live.href = `http://${room.ip}:${room.streamPort}/stream`;
    live.style.display = '';
  } else live.style.display = 'none';
  if (room.ip) { dev.href = `http://${room.ip}/`; dev.style.display = ''; }
  else dev.style.display = 'none';
  $('#mCamNote').textContent = room.ip
    ? '라이브 영상은 회의실과 같은 네트워크에서만 열립니다. (스냅샷은 어디서나 보입니다)'
    : '기기 IP 정보가 아직 없습니다.';

  $('#mState').innerHTML =
    `<span class="badge s-${room.state}"><span class="dot"></span>${STATE_LABEL[room.state]}</span>
     <div class="mut small" style="margin-top:8px">
       마지막 움직임 ${ago(room.lastMotion)} · 스냅샷 ${ago(room.snapshotAgo)} ·
       신호 ${room.rssi ?? '-'} dBm · 펌웨어 ${esc(room.fw || '-')}
     </div>`;
  $('#mTimeline').innerHTML = timeline(room);

  $('#mList').innerHTML = room.reservations.length
    ? room.reservations.map((r) => {
        const live = room.nowMin >= r.start && room.nowMin < r.end && room.date === todayStr();
        const st = r.status === 'no_show' ? '노쇼' : room.nowMin >= r.end ? '종료' : live ? '진행중' : '예정';
        return `<li class="${live ? 'now' : ''}">
          <div><div class="t">${hm(r.start)} ~ ${hm(r.end)}</div>
          <div class="mut small">${esc(r.title)} · ${esc(r.user)} · ${st}${r.checkedIn ? ' · 사용확인' : ''}</div></div>
          <button class="ghost" data-cancel="${esc(r.id)}" data-owner="${esc(r.user)}">취소</button></li>`;
      }).join('')
    : '<li class="mut">예약이 없습니다.</li>';
}

// ---------- 예약 ----------
async function submitBooking() {
  const room = ROOMS.find((r) => r.id === openRoomId);
  if (!room) return;
  const msg = $('#fMsg');
  msg.textContent = '';
  const body = new URLSearchParams({
    roomId: room.id,
    date: selectedDate,
    start: $('#fStart').value,
    end: $('#fEnd').value,
    title: $('#fTitle').value,
    user: $('#fUser').value,
  });
  try {
    await api('/api/reservations', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body,
    });
    msg.className = 'msg ok';
    msg.textContent = '예약이 등록되었습니다.';
    $('#fTitle').value = '';
    localStorage.setItem('roomUser', $('#fUser').value);
    await refresh();
    refreshEvents();
  } catch (e) {
    msg.className = 'msg err';
    msg.textContent = e.message;
  }
}

async function cancelReservation(id, owner) {
  const saved = localStorage.getItem('roomUser') || '';
  const who = owner ? prompt(`예약자 이름을 입력하면 취소됩니다. (예약자: ${owner})`, saved) : saved;
  if (who === null) return;
  try {
    await api(`/api/reservations/${encodeURIComponent(id)}/cancel`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ user: who }),
    });
    $('#fMsg').className = 'msg ok';
    $('#fMsg').textContent = '예약을 취소했습니다.';
    await refresh();
    refreshEvents();
  } catch (e) {
    $('#fMsg').className = 'msg err';
    $('#fMsg').textContent = e.message;
  }
}

// ---------- 이벤트 바인딩 ----------
document.addEventListener('click', (ev) => {
  const t = ev.target.closest('[data-open],[data-book],[data-room],[data-cancel]');
  if (!t) return;
  if (t.dataset.cancel) return cancelReservation(t.dataset.cancel, t.dataset.owner);
  if (t.dataset.book) {
    const now = new Date();
    const start = Math.ceil((now.getHours() * 60 + now.getMinutes()) / 30) * 30;
    $('#fStart').value = hm(Math.min(start, CFG.closeMinute - 30));
    $('#fEnd').value = hm(Math.min(start + 60, CFG.closeMinute));
    $('#fUser').value = localStorage.getItem('roomUser') || '';
    return openModal(t.dataset.book, true);
  }
  openModal(t.dataset.open || t.dataset.room, false);
});

$('#mClose').addEventListener('click', closeModal);
$('#modalBack').addEventListener('click', (e) => { if (e.target === $('#modalBack')) closeModal(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeModal(); });
$('#fSubmit').addEventListener('click', submitBooking);
$('#date').addEventListener('change', () => { selectedDate = $('#date').value || todayStr(); refresh(); });
$('#btnToday').addEventListener('click', () => { selectedDate = todayStr(); $('#date').value = selectedDate; refresh(); });

// ---------- 시작 ----------
(async function init() {
  $('#date').value = selectedDate;
  try { CFG = await api('/api/config'); } catch { /* 기본값 사용 */ }
  $('#fUser').value = localStorage.getItem('roomUser') || '';
  await refresh();
  await refreshEvents();
  setInterval(refresh, 5000);
  setInterval(refreshEvents, 15000);
})();
