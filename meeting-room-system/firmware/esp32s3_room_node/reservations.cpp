#include "reservations.h"
#include <Preferences.h>
#include <esp_random.h>

static Reservation s_list[MAX_RESERVATIONS];
static int         s_count = 0;
static bool        s_autoSave = true;
static Preferences resPrefs;
static const char *RES_NS = "roomres";

static void copyStr(char *dst, size_t cap, const String &src) {
  String s = src;
  s.replace("|", "/");
  s.replace("\n", " ");
  s.replace("\r", " ");
  s.trim();
  strncpy(dst, s.c_str(), cap - 1);
  dst[cap - 1] = '\0';
}

void reservationsBegin() {
  resPrefs.begin(RES_NS, true);
  size_t len = resPrefs.getBytesLength("list");
  if (len > 0 && len <= sizeof(s_list)) {
    resPrefs.getBytes("list", s_list, len);
    s_count = len / sizeof(Reservation);
  } else {
    s_count = 0;
  }
  resPrefs.end();
  if (s_count < 0 || s_count > MAX_RESERVATIONS) s_count = 0;
}

void reservationsSetAutoSave(bool on) { s_autoSave = on; }

void reservationsSave() {
  if (!s_autoSave) return;          // 일괄 변경 중에는 쓰기를 미룬다
  resPrefs.begin(RES_NS, false);
  if (s_count > 0) {
    resPrefs.putBytes("list", s_list, s_count * sizeof(Reservation));
  } else {
    resPrefs.remove("list");
  }
  resPrefs.end();
}

int reservationCount() { return s_count; }

const Reservation *reservationAt(int i) {
  if (i < 0 || i >= s_count) return nullptr;
  return &s_list[i];
}

static int indexOfId(const String &id) {
  for (int i = 0; i < s_count; i++) {
    if (id.equals(s_list[i].id)) return i;
  }
  return -1;
}

String reservationMakeLocalId() {
  char buf[20];
  snprintf(buf, sizeof(buf), "L%08lX", (unsigned long)(millis() ^ esp_random()));
  return String(buf);
}

bool reservationAdd(uint32_t ymd, uint16_t startMin, uint16_t endMin,
                    const String &title, const String &user,
                    bool synced, const String &forcedId, String &err) {
  if (ymd == 0) { err = "날짜가 올바르지 않습니다."; return false; }
  if (endMin <= startMin) { err = "종료 시각이 시작 시각보다 빨라요."; return false; }
  if (endMin > 1440) { err = "종료 시각이 24:00 을 넘을 수 없습니다."; return false; }

  // 같은 id 가 이미 있으면 갱신으로 처리
  if (forcedId.length()) {
    int ex = indexOfId(forcedId);
    if (ex >= 0) {
      s_list[ex].ymd = ymd;
      s_list[ex].startMin = startMin;
      s_list[ex].endMin = endMin;
      copyStr(s_list[ex].title, sizeof(s_list[ex].title), title);
      copyStr(s_list[ex].user, sizeof(s_list[ex].user), user);
      s_list[ex].flags = synced ? RES_FLAG_SYNCED : 0;
      reservationsSave();
      return true;
    }
  }

  for (int i = 0; i < s_count; i++) {
    if (s_list[i].ymd != ymd) continue;
    if (s_list[i].flags & RES_FLAG_CANCELED) continue;
    if (startMin < s_list[i].endMin && endMin > s_list[i].startMin) {
      err = "이미 예약된 시간과 겹칩니다.";
      return false;
    }
  }

  if (s_count >= MAX_RESERVATIONS) {
    err = "기기에 저장 가능한 예약 수를 초과했습니다.";
    return false;
  }

  Reservation &r = s_list[s_count];
  memset(&r, 0, sizeof(r));
  copyStr(r.id, sizeof(r.id), forcedId.length() ? forcedId : reservationMakeLocalId());
  copyStr(r.title, sizeof(r.title), title.length() ? title : String("회의"));
  copyStr(r.user, sizeof(r.user), user.length() ? user : String("익명"));
  r.ymd = ymd;
  r.startMin = startMin;
  r.endMin = endMin;
  r.flags = synced ? RES_FLAG_SYNCED : 0;
  s_count++;
  reservationsSave();
  return true;
}

bool reservationCancel(const String &id) {
  int i = indexOfId(id);
  if (i < 0) return false;
  if (s_list[i].flags & RES_FLAG_SYNCED) {
    // 서버에 반영된 예약은 취소 표시만 하고, 다음 동기화 때 서버 목록으로 정리된다.
    s_list[i].flags |= RES_FLAG_CANCELED;
    reservationsSave();
    return true;
  }
  return reservationRemove(id);
}

bool reservationRemove(const String &id) {
  int i = indexOfId(id);
  if (i < 0) return false;
  for (int j = i; j < s_count - 1; j++) s_list[j] = s_list[j + 1];
  s_count--;
  reservationsSave();
  return true;
}

const Reservation *reservationCurrent(uint32_t ymd, uint16_t nowMin) {
  for (int i = 0; i < s_count; i++) {
    const Reservation &r = s_list[i];
    if (r.ymd != ymd || (r.flags & RES_FLAG_CANCELED)) continue;
    if (nowMin >= r.startMin && nowMin < r.endMin) return &r;
  }
  return nullptr;
}

const Reservation *reservationNext(uint32_t ymd, uint16_t nowMin) {
  const Reservation *best = nullptr;
  for (int i = 0; i < s_count; i++) {
    const Reservation &r = s_list[i];
    if (r.ymd != ymd || (r.flags & RES_FLAG_CANCELED)) continue;
    if (r.startMin <= nowMin) continue;
    if (!best || r.startMin < best->startMin) best = &r;
  }
  return best;
}

void reservationsClearSyncedFor(uint32_t ymd) {
  int w = 0;
  for (int i = 0; i < s_count; i++) {
    bool drop = (s_list[i].ymd == ymd) && (s_list[i].flags & RES_FLAG_SYNCED);
    if (!drop) {
      if (w != i) s_list[w] = s_list[i];
      w++;
    }
  }
  s_count = w;
}

int reservationFirstPendingIndex() {
  for (int i = 0; i < s_count; i++) {
    if (!(s_list[i].flags & RES_FLAG_SYNCED) && !(s_list[i].flags & RES_FLAG_CANCELED)) return i;
  }
  return -1;
}

void reservationMarkSynced(int index, const String &serverId) {
  if (index < 0 || index >= s_count) return;
  if (serverId.length()) copyStr(s_list[index].id, sizeof(s_list[index].id), serverId);
  s_list[index].flags |= RES_FLAG_SYNCED;
  reservationsSave();
}

String reservationsFingerprint(uint32_t ymd) {
  String fp;
  fp.reserve(256);
  for (int i = 0; i < s_count; i++) {
    const Reservation &r = s_list[i];
    if (r.ymd != ymd || !(r.flags & RES_FLAG_SYNCED)) continue;
    fp += r.id; fp += '|';
    fp += String(r.startMin); fp += '|';
    fp += String(r.endMin); fp += '|';
    fp += r.title; fp += '|';
    fp += r.user; fp += '\n';
  }
  return fp;
}

void reservationsPurgeBefore(uint32_t ymd) {
  if (ymd == 0) return;
  int w = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_list[i].ymd >= ymd) {
      if (w != i) s_list[w] = s_list[i];
      w++;
    }
  }
  if (w != s_count) {
    s_count = w;
    reservationsSave();
  }
}
