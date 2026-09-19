/*
 * timeutil.h - NTP 시각 및 날짜/시간 문자열 변환
 */
#pragma once
#include <Arduino.h>

void     timeBegin();                 // NTP 시작 (Wi-Fi STA 연결 후 호출)
bool     timeIsSynced();              // 2021년 이후 시각을 받았는지
uint32_t timeTodayYmd();              // 20260919 형태
String   timeTodayString();           // "2026-09-19"
uint16_t timeNowMinutes();            // 자정으로부터의 분 (0~1439)
String   timeNowString();             // "14:05"
String   timeNowFull();               // "2026-09-19 14:05:30"

String   ymdToString(uint32_t ymd);   // 20260919 -> "2026-09-19"
uint32_t stringToYmd(const String &s);// "2026-09-19" -> 20260919 (실패 시 0)
int      parseHm(const String &s);    // "14:30" -> 870 (실패 시 -1)
String   minutesToHm(uint16_t m);     // 870 -> "14:30"
