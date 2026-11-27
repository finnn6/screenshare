#pragma once
#include <windows.h>
#include <cstdint>

// 영상과 소리가 같은 기준 시각을 쓰도록 하는 시계.
// QPC를 100ns 단위로 바꿔 쓴다. WGC 프레임 시각과 WASAPI 패킷 시각이 모두 이 단위라서 그대로 비교할 수 있다.
inline int64_t Qpc100ns() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq * 10'000'000 + c.QuadPart % freq * 10'000'000 / freq;
}

// 스트림 기준점. 처음 부른 시각으로 고정된다 (main에서 캡처 시작 전에 한 번 불러 둔다)
inline int64_t MediaEpoch100ns() {
    static const int64_t epoch = Qpc100ns();
    return epoch;
}

// 기준점부터 흐른 시간(초). RTP 타임스탬프 계산에 쓴다
inline double ToMediaSeconds(int64_t t100ns) { return double(t100ns - MediaEpoch100ns()) / 1e7; }
inline double MediaSecondsNow() { return ToMediaSeconds(Qpc100ns()); }
