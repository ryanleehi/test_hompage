'use strict';

/** 자정으로부터의 분 <-> "HH:MM" */
function toMinutes(value) {
  if (value === null || value === undefined || value === '') return NaN;
  if (typeof value === 'number') return Math.trunc(value);
  const s = String(value).trim();
  if (/^\d+$/.test(s)) return parseInt(s, 10);
  const m = s.match(/^(\d{1,2}):(\d{2})$/);
  if (!m) return NaN;
  const h = parseInt(m[1], 10);
  const mi = parseInt(m[2], 10);
  if (h < 0 || h > 24 || mi < 0 || mi > 59) return NaN;
  return h * 60 + mi;
}

function toHm(minutes) {
  const m = Math.max(0, Math.min(1440, Math.trunc(minutes)));
  return String(Math.floor(m / 60)).padStart(2, '0') + ':' + String(m % 60).padStart(2, '0');
}

/** 서버 로컬 타임존 기준 "YYYY-MM-DD" */
function todayString(d = new Date()) {
  return (
    d.getFullYear() +
    '-' + String(d.getMonth() + 1).padStart(2, '0') +
    '-' + String(d.getDate()).padStart(2, '0')
  );
}

function nowMinutes(d = new Date()) {
  return d.getHours() * 60 + d.getMinutes();
}

function isValidDate(s) {
  return typeof s === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(s);
}

function clean(value, maxLen) {
  return String(value === undefined || value === null ? '' : value)
    .replace(/[\r\n|]/g, ' ')
    .trim()
    .slice(0, maxLen);
}

function makeId(prefix = 'R') {
  return prefix + Date.now().toString(36).toUpperCase() + Math.random().toString(36).slice(2, 6).toUpperCase();
}

/** 두 구간이 겹치는지 */
function overlaps(aStart, aEnd, bStart, bEnd) {
  return aStart < bEnd && aEnd > bStart;
}

function sendJson(res, code, payload) {
  const body = JSON.stringify(payload);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
    'Access-Control-Allow-Origin': '*',
  });
  res.end(body);
}

function sendText(res, code, text) {
  const body = String(text);
  res.writeHead(code, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

/** 요청 본문 읽기 (최대 크기 제한) */
function readBody(req, limitBytes = 512 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on('data', (c) => {
      total += c.length;
      if (total > limitBytes) {
        reject(Object.assign(new Error('payload too large'), { statusCode: 413 }));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

/** JSON 또는 form-urlencoded 본문을 객체로 */
async function readParams(req) {
  const raw = await readBody(req);
  const type = (req.headers['content-type'] || '').toLowerCase();
  const text = raw.toString('utf8');
  if (type.includes('application/json')) {
    try {
      return JSON.parse(text || '{}');
    } catch {
      throw Object.assign(new Error('잘못된 JSON 형식입니다.'), { statusCode: 400 });
    }
  }
  const out = {};
  for (const [k, v] of new URLSearchParams(text)) out[k] = v;
  return out;
}

function agoSeconds(iso) {
  if (!iso) return -1;
  return Math.max(0, Math.round((Date.now() - new Date(iso).getTime()) / 1000));
}

module.exports = {
  toMinutes, toHm, todayString, nowMinutes, isValidDate, clean, makeId,
  overlaps, sendJson, sendText, readBody, readParams, agoSeconds,
};
