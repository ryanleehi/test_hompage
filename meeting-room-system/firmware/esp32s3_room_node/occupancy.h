/*
 * occupancy.h - PIR 센서 기반 재실 판정
 */
#pragma once
#include <Arduino.h>

void occupancyBegin();
void occupancyLoop();

bool     occupancyIsOccupied();     // 사용중 여부 (마지막 감지 후 OCCUPANCY_HOLD_MS 유지)
bool     occupancyRawMotion();      // 현재 순간의 센서 값
uint32_t occupancySecondsSinceMotion();  // 마지막 감지 후 경과 초 (감지 이력 없으면 0xFFFFFFFF)
uint32_t occupancyMotionCount();    // 부팅 후 누적 감지 횟수
bool     occupancyWarmingUp();      // PIR 안정화 대기 중인지
