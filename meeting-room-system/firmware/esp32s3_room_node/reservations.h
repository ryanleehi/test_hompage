/*
 * reservations.h - 기기에 저장되는 예약 목록
 *
 * 중앙 서버가 있으면 서버가 원본이고 기기는 캐시 역할을 한다.
 * 서버가 없거나 통신이 끊겨도 기기 단독으로 예약을 받을 수 있게
 * "미동기화(pending)" 예약을 별도로 보관했다가 나중에 밀어 올린다.
 */
#pragma once
#include <Arduino.h>
#include "config.h"

#define RES_FLAG_SYNCED    0x01   // 서버에 반영 완료
#define RES_FLAG_CANCELED  0x02   // 취소됨 (서버에 취소 전달 대기 포함)

struct Reservation {
  char     id[20];
  char     title[41];
  char     user[25];
  uint32_t ymd;        // 20260919
  uint16_t startMin;   // 자정으로부터 분
  uint16_t endMin;
  uint8_t  flags;
};

void reservationsBegin();
void reservationsSave();
// 여러 건을 한꺼번에 바꿀 때 NVS 쓰기를 모으기 위한 스위치
void reservationsSetAutoSave(bool on);

// 특정 날짜의 "서버에서 받은" 예약들을 한 줄 문자열로 만든다. (변경 감지용)
String reservationsFingerprint(uint32_t ymd);

int  reservationCount();
const Reservation *reservationAt(int i);

// 추가. 성공하면 true, 실패하면 err 에 사유(한국어)를 채운다.
bool reservationAdd(uint32_t ymd, uint16_t startMin, uint16_t endMin,
                    const String &title, const String &user,
                    bool synced, const String &forcedId, String &err);

bool reservationCancel(const String &id);
bool reservationRemove(const String &id);

const Reservation *reservationCurrent(uint32_t ymd, uint16_t nowMin);
const Reservation *reservationNext(uint32_t ymd, uint16_t nowMin);

// 서버 동기화용
void reservationsClearSyncedFor(uint32_t ymd);
int  reservationFirstPendingIndex();          // 서버에 아직 못 올린 예약 (없으면 -1)
void reservationMarkSynced(int index, const String &serverId);
void reservationsPurgeBefore(uint32_t ymd);   // 지난 날짜 정리

String reservationMakeLocalId();
