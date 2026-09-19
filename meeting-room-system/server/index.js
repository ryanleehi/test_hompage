#!/usr/bin/env node
'use strict';
/**
 * 회의실 통합 대시보드 서버
 *
 *  - 여러 대의 ESP32-S3-CAM 노드에서 재실 상태/스냅샷을 받아 모아 보여준다.
 *  - 브라우저에서 회의실 예약을 만들고 취소한다.
 *  - 외부 의존성 없이 Node 18+ 에서 `node index.js` 로 바로 실행된다.
 */
const http = require('http');
const fs = require('fs');
const path = require('path');

const cfg = require('./lib/config');
const { Store } = require('./lib/store');
const { handleApi } = require('./lib/api');
const { runMaintenance } = require('./lib/logic');

const store = new Store(cfg.DATA_DIR);
const PUBLIC_DIR = path.join(__dirname, 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
};

function serveStatic(req, res, pathname) {
  let rel = decodeURIComponent(pathname);
  if (rel === '/' || rel === '') rel = '/index.html';

  const target = path.join(PUBLIC_DIR, rel);
  // 경로 탈출 방지
  if (!target.startsWith(PUBLIC_DIR + path.sep) && target !== path.join(PUBLIC_DIR, 'index.html')) {
    res.writeHead(403).end('forbidden');
    return;
  }
  fs.readFile(target, (err, buf) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('페이지를 찾을 수 없습니다.');
      return;
    }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(target).toLowerCase()] || 'application/octet-stream',
      'Content-Length': buf.length,
      'Cache-Control': 'no-cache',
    });
    res.end(buf);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  try {
    const handled = await handleApi(req, res, url, store);
    if (handled) return;
    serveStatic(req, res, url.pathname);
  } catch (err) {
    const code = err.statusCode || 500;
    if (!res.headersSent) {
      res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ ok: false, error: err.message || '서버 오류' }));
    }
    if (code >= 500) console.error('[server]', err);
  }
});

// 재실/노쇼 판정은 요청과 무관하게 주기적으로도 돌린다.
setInterval(() => {
  try {
    runMaintenance(store);
  } catch (err) {
    console.error('[maintenance]', err.message);
  }
}, 30000);

// 하루에 한 번 오래된 예약 정리
setInterval(() => store.purgeOld(), 6 * 3600 * 1000);

function shutdown() {
  console.log('\n[server] 종료 중…');
  store.saveNow();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 2000).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

server.listen(cfg.PORT, cfg.HOST, () => {
  console.log('==============================================');
  console.log(` 회의실 통합 대시보드: http://localhost:${cfg.PORT}/`);
  console.log(` 데이터 폴더        : ${cfg.DATA_DIR}`);
  console.log(` 기기 토큰          : ${cfg.DEVICE_TOKEN === 'change-me-token' ? '기본값 사용 중 (운영 시 반드시 변경)' : '설정됨'}`);
  console.log(` 등록된 회의실      : ${store.rooms().length}개`);
  console.log('==============================================');
});
