/*
 * camera.h - OV2640 초기화 및 MJPEG 스트리밍 서버(포트 81)
 */
#pragma once
#include <Arduino.h>
#include "esp_camera.h"

bool cameraBegin();          // 실패해도 나머지 기능(예약/PIR)은 계속 동작한다
bool cameraReady();
void cameraStartServer();    // http://<ip>:81/stream , /capture
void cameraStopServer();

// 뮤텍스로 보호되는 프레임 획득/반납 (클라우드 스냅샷 업로드용)
camera_fb_t *cameraGrab(uint32_t timeoutMs = 2000);
void         cameraRelease(camera_fb_t *fb);

void cameraSetFrameSize(framesize_t fs);
