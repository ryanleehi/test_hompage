/*
 * cloud.h - 중앙 서버(통합 대시보드)와의 동기화
 *
 * 기기에 JSON 라이브러리를 요구하지 않으려고 아주 단순한
 * 파이프(|) 구분 텍스트 프로토콜을 쓴다. 브라우저용 API는 서버가 JSON으로 제공한다.
 */
#pragma once
#include <Arduino.h>

void     cloudBegin();
void     cloudLoop();

bool     cloudConfigured();            // serverUrl 이 설정되어 있는지
bool     cloudOnline();                // 최근 통신이 성공했는지
uint32_t cloudSecondsSinceSync();
String   cloudLastError();

void     cloudRequestPull();           // 다음 loop 에서 즉시 예약 목록 동기화
bool     cloudCancel(const String &id);

// 미동기화 예약을 서버로 올린 결과
enum CloudPushResult {
  CLOUD_PUSH_NONE = 0,      // 올릴 것이 없음
  CLOUD_PUSH_OK,            // 서버에 반영됨
  CLOUD_PUSH_OFFLINE,       // 통신 실패 - 로컬에 남겨두고 나중에 재시도
  CLOUD_PUSH_REJECTED,      // 서버가 거절(시간 겹침 등) - 로컬 예약은 삭제됨
};
CloudPushResult cloudPushPending();
String   cloudRejectReason();          // 마지막 거절 사유
