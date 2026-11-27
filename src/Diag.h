#pragma once

// 진단 모드. 실행할 때 --diag 를 주면 켜진다 (main에서 스레드를 시작하기 전에 정함).
// 켜면: test.h264 / audio_test.wav 저장(처음 10초), 소리 2초 통계, 소리 RTP 검사 출력
inline bool g_diag = false;
