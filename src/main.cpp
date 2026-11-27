#include "Window.h"
#include "Renderer.h"
#include "Capture.h"
#include "CaptureSources.h"
#include "Pipeline.h"
#include "Streamer.h"
#include "AudioCapture.h"
#include "MediaClock.h"
#include "Diag.h"
#include <timeapi.h>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

// 실행: screenshare.exe [방이름] [시그널링 주소] [최대 Mbps] [--diag]
//   예) screenshare.exe test ws://127.0.0.1:8765/ws 20
//   --diag: 진단 모드 (Diag.h 참고). 위치는 아무 데나 둬도 됨
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--diag") g_diag = true;
        else args.push_back(a);
    }
    const std::string room = args.size() > 0 ? args[0] : "test";
    const std::string url = args.size() > 1 ? args[1] : "ws://127.0.0.1:8765/ws";
    const UINT maxMbps = args.size() > 2 ? (UINT)std::max(1, std::atoi(args[2].c_str())) : 20;

    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (g_diag) std::cout << "진단 모드: 처음 10초 영상·소리 파일 저장, 소리 통계·RTP 검사 출력\n";

    if (!Capture::IsSupported()) {
        std::cout << "이 PC에서는 화면 캡처를 지원하지 않음\n";
        return 1;
    }

    // 1) 공유 대상 고르기
    auto sources = ListCaptureSources();
    std::cout << "공유할 대상을 고르세요\n";
    for (size_t i = 0; i < sources.size(); ++i) {
        std::cout << "  " << i << ") " << ToUtf8(sources[i].name) << "\n";
    }
    std::cout << "번호: ";
    size_t choice = 0;
    if (!(std::cin >> choice) || choice >= sources.size()) {
        std::cout << "잘못된 번호\n";
        return 1;
    }

    // 2) 미리보기 창
    Window window;
    if (!window.Create(L"화면공유 뷰어", 1280, 720)) {
        std::cout << "창 생성 실패\n";
        return 1;
    }
    Renderer renderer;
    if (!renderer.Init(window.Handle())) return 1;

    // 3) 시그널링 서버 접속 (시청자가 들어오면 WebRTC 연결을 만든다)
    Streamer streamer;
    streamer.Start(url, room, maxMbps * 1000);

    // 영상·소리 RTP 시각의 공통 기준점을 캡처 시작 전에 정해 둔다
    MediaEpoch100ns();

    // 4) 캡처·인코딩 스레드 시작 (인코딩한 프레임을 streamer로 전송)
    Pipeline pipeline;
    pipeline.Start(sources[choice], 165, maxMbps, &streamer);

    // 4-1) 소리 캡처·인코딩 스레드
    //   창 공유: 그 창을 띄운 프로그램(+하위 프로세스) 소리만 / 모니터 공유: PC 전체 소리
    DWORD audioPid = 0;
    if (sources[choice].hwnd) GetWindowThreadProcessId(sources[choice].hwnd, &audioPid);
    AudioPipeline audio;
    audio.Start(&streamer, audioPid);

    // 5) 메인 스레드: 공유 버퍼에서 최신 화면을 읽어 미리보기
    bool failed = false;
    while (window.ProcessMessages()) {
        if (!pipeline.IsRunning()) {
            failed = true;
            break;
        }

        UINT w, h;
        if (window.ConsumeResize(w, h)) renderer.Resize(w, h);

        UINT pw = 0, ph = 0;
        ID3D11ShaderResourceView* srv = pipeline.Preview().BeginRead(renderer.Device(), pw, ph);
        renderer.Draw(srv, pw, ph);
        pipeline.Preview().EndRead();
        renderer.Present();
    }

    audio.Stop();
    pipeline.Stop();      // 인코딩 스레드가 멈춘 뒤에
    streamer.Stop();      // 시청자 연결을 닫는다
    pipeline.Preview().ReleaseReader();
    timeEndPeriod(1);

    if (failed) {
        std::cout << "오류로 종료됨. 엔터를 누르면 닫힙니다.\n";
        std::cin.ignore();
        std::cin.get();
    }
    return 0;
}
