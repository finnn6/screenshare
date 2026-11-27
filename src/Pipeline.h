#pragma once
#include "CaptureSources.h"
#include "PreviewShare.h"
#include <atomic>
#include <thread>

class Streamer;

// 캡처 → 인코딩을 별도 스레드에서 돌림
class Pipeline {
public:
    ~Pipeline() { Stop(); }

    // streamer가 있으면 인코딩한 프레임을 시청자들에게 보낸다 (nullptr이면 미리보기만)
    // maxMbps: 비트레이트 상한. 네트워크(특히 와이파이)로 보낼 때는 너무 높으면 패킷이 손실된다
    bool Start(const CaptureSource& source, UINT fps, UINT maxMbps, Streamer* streamer = nullptr);
    void Stop();

    bool IsRunning() const { return m_running; }

    // 미리보기 창이 인코딩 화면을 읽어갈 공유 버퍼
    PreviewShare& Preview() { return m_preview; }

private:
    void ThreadMain();
    void Run();

    CaptureSource m_source;
    UINT m_fps = 60;
    UINT m_maxKbps = 20000;
    Streamer* m_streamer = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{ false };
    std::atomic<bool> m_running{ false };
    PreviewShare m_preview;
};