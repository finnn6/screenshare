#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

class Streamer;

// 소리를 WASAPI 루프백으로 캡처해서 Opus로 인코딩하고 Streamer로 보낸다.
//  - targetPid가 있으면 그 프로그램(+하위 프로세스)의 소리만 캡처 (프로세스 루프백, 윈도우 10 2004 이상)
//  - 없거나 지원하지 않으면 기본 출력 장치로 나가는 소리 전체
// 기본 출력 장치가 바뀌면(헤드셋 연결 등) 1초 뒤 다시 연결한다.
class AudioPipeline {
public:
    ~AudioPipeline() { Stop(); }

    bool Start(Streamer* streamer, DWORD targetPid = 0);
    void Stop();

private:
    void ThreadMain();
    bool Run();   // false면 다시 시도하지 않음 (지원하지 않는 장치 형식)

    Streamer* m_streamer = nullptr;
    DWORD m_targetPid = 0;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{ false };
};
