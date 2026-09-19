#include "timeutil.h"
#include "config.h"
#include <time.h>

void timeBegin() {
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
}

static bool getTm(struct tm &out) {
  time_t now = time(nullptr);
  if (now < 1609459200) return false;   // 2021-01-01 이전이면 아직 동기화 안 됨
  localtime_r(&now, &out);
  return true;
}

bool timeIsSynced() {
  struct tm t;
  return getTm(t);
}

uint32_t timeTodayYmd() {
  struct tm t;
  if (!getTm(t)) return 0;
  return (uint32_t)(t.tm_year + 1900) * 10000UL + (uint32_t)(t.tm_mon + 1) * 100UL + (uint32_t)t.tm_mday;
}

String timeTodayString() { return ymdToString(timeTodayYmd()); }

uint16_t timeNowMinutes() {
  struct tm t;
  if (!getTm(t)) return 0;
  return (uint16_t)(t.tm_hour * 60 + t.tm_min);
}

String timeNowString() { return minutesToHm(timeNowMinutes()); }

String timeNowFull() {
  struct tm t;
  if (!getTm(t)) return String("시각 미동기화");
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String ymdToString(uint32_t ymd) {
  if (ymd == 0) return String("");
  char buf[12];
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
           (unsigned)(ymd / 10000), (unsigned)((ymd / 100) % 100), (unsigned)(ymd % 100));
  return String(buf);
}

uint32_t stringToYmd(const String &s) {
  if (s.length() < 10) return 0;
  int y = s.substring(0, 4).toInt();
  int m = s.substring(5, 7).toInt();
  int d = s.substring(8, 10).toInt();
  if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return (uint32_t)y * 10000UL + (uint32_t)m * 100UL + (uint32_t)d;
}

int parseHm(const String &s) {
  int c = s.indexOf(':');
  if (c < 1) return -1;
  int h = s.substring(0, c).toInt();
  int m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

String minutesToHm(uint16_t m) {
  if (m > 1440) m = 1440;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
  return String(buf);
}
