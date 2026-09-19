'use strict';
const path = require('path');

/** 모든 설정은 환경변수로 덮어쓸 수 있다. (.env 파일 대신 실행 시 지정) */
module.exports = {
  PORT: parseInt(process.env.PORT || '3000', 10),
  HOST: process.env.HOST || '0.0.0.0',
  DATA_DIR: process.env.DATA_DIR || path.join(__dirname, '..', 'data'),

  // ESP32 기기가 보내는 X-Device-Token 과 일치해야 한다.
  DEVICE_TOKEN: process.env.DEVICE_TOKEN || 'change-me-token',
  // 값을 넣으면 예약 삭제/기기 삭제 같은 관리 API에 X-Admin-Token 이 필요해진다.
  ADMIN_TOKEN: process.env.ADMIN_TOKEN || '',

  // 하트비트가 이 시간 이상 끊기면 기기를 오프라인으로 본다.
  OFFLINE_AFTER_SEC: parseInt(process.env.OFFLINE_AFTER_SEC || '60', 10),
  // 예약 시작 후 이 시간까지 사람이 감지되지 않으면 노쇼 처리한다.
  NO_SHOW_MINUTES: parseInt(process.env.NO_SHOW_MINUTES || '10', 10),
  // 노쇼 예약을 자동으로 비워서 다른 사람이 쓸 수 있게 할지.
  AUTO_RELEASE: (process.env.AUTO_RELEASE || 'true') !== 'false',
  // 예약 가능 시간대 (분)
  OPEN_MINUTE: parseInt(process.env.OPEN_MINUTE || '480', 10),    // 08:00
  CLOSE_MINUTE: parseInt(process.env.CLOSE_MINUTE || '1320', 10), // 22:00
};
