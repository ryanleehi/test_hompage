#include "occupancy.h"
#include "config.h"

static uint32_t s_lastMotionMs = 0;
static bool     s_everDetected = false;
static uint32_t s_motionCount  = 0;
static bool     s_lastRaw      = false;
static uint32_t s_lastSampleMs = 0;
static uint32_t s_bootMs       = 0;

void occupancyBegin() {
#if PIR_ACTIVE_HIGH
  pinMode(PIR_PIN, INPUT);        // HC-SR501 은 자체 풀다운 출력
#else
  pinMode(PIR_PIN, INPUT_PULLUP);
#endif
  s_bootMs = millis();
}

bool occupancyWarmingUp() {
  return (millis() - s_bootMs) < PIR_WARMUP_MS;
}

bool occupancyRawMotion() {
  int v = digitalRead(PIR_PIN);
#if PIR_ACTIVE_HIGH
  return v == HIGH;
#else
  return v == LOW;
#endif
}

void occupancyLoop() {
  uint32_t now = millis();
  if (now - s_lastSampleMs < 50) return;   // 20Hz 샘플링
  s_lastSampleMs = now;

  if (occupancyWarmingUp()) return;        // 전원 인가 직후 오검출 구간은 무시

  bool raw = occupancyRawMotion();

  if (raw) {
    // 상승 에지에서만 카운트하되, 채터링은 디바운스로 거른다.
    if (!s_lastRaw && (now - s_lastMotionMs) > MOTION_DEBOUNCE_MS) {
      s_motionCount++;
    }
    s_lastMotionMs = now;
    s_everDetected = true;
  }
  s_lastRaw = raw;
}

bool occupancyIsOccupied() {
  if (!s_everDetected) return false;
  return (millis() - s_lastMotionMs) < OCCUPANCY_HOLD_MS;
}

uint32_t occupancySecondsSinceMotion() {
  if (!s_everDetected) return 0xFFFFFFFFUL;
  return (millis() - s_lastMotionMs) / 1000UL;
}

uint32_t occupancyMotionCount() { return s_motionCount; }
