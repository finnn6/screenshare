#include "Pipeline.h"
#include "Streamer.h"
#include "Capture.h"
#include "Encoder.h"
#include "Scaler.h"
#include "MediaClock.h"
#include "Diag.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;


// 공유 대상 크기에 맞는 인코더 설정 만들기
static EncoderSettings MakeSettings(UINT srcW, UINT srcH, UINT fps, UINT maxKbps) {
    EncoderSettings s;
    s.width  = std::clamp<UINT>((srcW + 1) & ~1u, 160, 4096);
    s.height = std::clamp<UINT>((srcH + 1) & ~1u, 90, 4096);
    s.fps = fps;
    double pixels = double(s.width) * s.height;
    double kbps = 20000.0 * (pixels / (1920.0 * 1080.0)) * (fps / 60.0);
    s.bitrateKbps = (UINT)std::clamp(kbps, 3000.0, std::max(3000.0, double(maxKbps)));
    return s;
}

static void PrintSettings(const EncoderSettings& s) {
    std::cout << "인코딩: H.264 " << s.width << "x" << s.height
              << " " << s.fps << "fps | 목표 화질 " << s.quality
              << " | 최대 " << s.bitrateKbps / 1000 << "Mbps\n";
}

bool Pipeline::Start(const CaptureSource& source, UINT fps, UINT maxMbps, Streamer* streamer) {
    Stop();
    m_source = source;
    m_fps = fps;
    m_maxKbps = maxMbps * 1000;
    m_streamer = streamer;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&Pipeline::ThreadMain, this);
    return true;
}

void Pipeline::Stop() {
    m_stopRequested = true;
    if (m_thread.joinable()) m_thread.join();
}

void Pipeline::ThreadMain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        Run();
    }
    catch (const winrt::hresult_error& e) {
        std::cout << "인코딩 스레드 오류: 0x" << std::hex << e.code().value << std::dec
                  << " " << winrt::to_string(e.message()) << "\n";
    }
    catch (const std::exception& e) {
        std::cout << "인코딩 스레드 오류: " << e.what() << "\n";
    }
    m_preview.ReleaseWriter();
    m_running = false;
    winrt::uninit_apartment();
}

void Pipeline::Run() {
    using Clock = std::chrono::steady_clock;

    // 1) 이 스레드 전용 D3D11 장치
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context);
    if (FAILED(hr)) {
        std::cout << "인코딩용 D3D11 장치 생성 실패: 0x" << std::hex << hr << std::dec << "\n";
        return;
    }

    Scaler scaler;
    if (!scaler.Init(device.Get())) return;

    // 2) 캡처
    Capture capture;
    bool ok = m_source.hwnd
        ? capture.StartWindow(m_source.hwnd, device.Get())
        : capture.StartMonitor(m_source.hmon, device.Get());
    if (!ok) return;
    std::cout << "공유 중: " << ToUtf8(m_source.name)
              << " (" << capture.Width() << "x" << capture.Height() << ")\n";

    // 3) 인코더
    Encoder encoder;
    if (!encoder.Init(device.Get())) return;
    EncoderSettings settings = MakeSettings(capture.Width(), capture.Height(), m_fps, m_maxKbps);
    if (!encoder.Start(settings)) return;
    PrintSettings(settings);

    // 4) 루프 준비
    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000 / m_fps);
    const auto resizeDelay = std::chrono::milliseconds(300);
    auto lastEncode = Clock::now() - frameInterval;
    bool hasNewFrame = false;
    bool hasAnyFrame = false;        // 캡처 프레임을 한 번이라도 받았는지
    bool keyframePending = false;    // 새 시청자 입장 / PLI로 키프레임이 필요한지
    const auto idleRepeat = std::chrono::milliseconds(500);

    // 키프레임은 크기가 커서 그 자체로 패킷 손실을 부르기 쉽다.
    // 손실 → PLI → 키프레임 → 또 손실... 이 반복되지 않도록 키프레임 사이에 최소 간격을 둔다.
    const auto keyframeMinGap = std::chrono::milliseconds(1000);
    auto lastKeyframe = Clock::now() - keyframeMinGap;

    UINT resizeW = 0, resizeH = 0;
    auto resizeSince = Clock::now();

    // 테스트용: 처음 10초를 파일로 저장
    // (진단 모드에서만)
    std::ofstream dumpFile;
    const auto dumpDuration = std::chrono::seconds(10);
    const auto dumpStart = Clock::now();
    bool dumping = g_diag;
    if (dumping) {
        dumpFile.open("test.h264", std::ios::binary);
        std::cout << "영상 저장 위치: " << std::filesystem::absolute("test.h264").string() << "\n";
    }

    // 측정용: GPU 작업 완료 신호
    ComPtr<ID3D11Query> gpuDone;
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    device->CreateQuery(&qd, &gpuDone);

    std::vector<uint8_t> packet;

    auto statsStart = Clock::now();
    uint64_t statFrames = 0, statBytes = 0, statKeyframes = 0;
    double statEncodeMs = 0, statGpuMs = 0;
    // 지연 측정: 캡처된 시각 기준 경과 시간 (새 프레임일 때만 집계)
    uint64_t statAgeFrames = 0;
    double statAgeStartMs = 0, statAgeSendMs = 0, statAgeSendMaxMs = 0;

    // 5) 루프
    while (!m_stopRequested) {
        // 새 프레임이 오거나, 다음 인코딩 시각이 될 때까지 잠들어 있음 (최대 100ms)
        DWORD timeoutMs = 100;
        if (hasNewFrame) {
            auto remain = frameInterval - (Clock::now() - lastEncode);
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(remain).count();
            timeoutMs = ms > 0 ? (DWORD)ms : 0;
        }
        WaitForSingleObject(capture.FrameEvent(), timeoutMs);

        // 쌓인 프레임을 전부 꺼내서 가장 최신 것만 남김
        while (capture.Update(context.Get())) hasNewFrame = hasAnyFrame = true;
        auto now = Clock::now();

        // 공유 대상 크기가 바뀌었으면, 0.3초 동안 안정된 뒤 인코더 재시작
        if (capture.Width() > 0) {
            EncoderSettings wanted = MakeSettings(capture.Width(), capture.Height(), m_fps, m_maxKbps);
            if (wanted.width != settings.width || wanted.height != settings.height) {
                if (wanted.width != resizeW || wanted.height != resizeH) {
                    resizeW = wanted.width;
                    resizeH = wanted.height;
                    resizeSince = now;
                }
                else if (now - resizeSince >= resizeDelay) {
                    settings = wanted;
                    if (encoder.Start(settings)) {
                        PrintSettings(settings);
                        hasNewFrame = true;
                    }
                    else {
                        std::cout << "인코더 재시작 실패\n";
                    }
                }
            }
        }

        // 새 시청자 입장 / PLI 요청을 받아 둔다
        if (m_streamer && m_streamer->ConsumeKeyframeRequest()) keyframePending = true;

        // 화면이 멈춰 있으면 캡처 프레임이 안 와서 아무것도 안 보내게 된다.
        // 그래서 키프레임 요청이 있거나 0.5초 동안 보낸 게 없으면, 마지막 화면을 다시 인코딩한다.
        // (멈춘 화면에 들어온 시청자가 검은 화면만 보는 문제 방지)
        const bool keyframeReady = keyframePending && now - lastKeyframe >= keyframeMinGap;
        const bool resend = hasAnyFrame && (keyframeReady || now - lastEncode >= idleRepeat);

        // 새 화면(또는 다시 보낼 화면)이 있고, 목표 fps 간격이 지났으면 인코딩
        if ((hasNewFrame || resend) && encoder.IsStarted() && now - lastEncode >= frameInterval) {
            const bool freshFrame = hasNewFrame;   // 재전송(resend)이 아닌 진짜 새 프레임인지
            hasNewFrame = false;
            lastEncode += frameInterval;                              // 예정 시각 기준
            if (now - lastEncode > frameInterval) lastEncode = now;   // 너무 밀렸으면 다시 맞춤

            // 측정: 캡처 → 처리 시작까지 (프레임 간격 대기 + 큐잉)
            const double ageStartMs = (Qpc100ns() - capture.LastFrameTime100ns()) / 1e4;

            scaler.DrawFit(context.Get(), encoder.InputRTV(), settings.width, settings.height,
                           capture.SRV(), capture.Width(), capture.Height(), false);

            // 미리보기용으로 공유 버퍼에 올리기
            m_preview.Publish(device.Get(), context.Get(), encoder.InputTexture(),
                              settings.width, settings.height);

            // 측정: GPU 준비 시간
            auto tg = Clock::now();
            context->End(gpuDone.Get());
            context->Flush();
            while (context->GetData(gpuDone.Get(), nullptr, 0, 0) == S_FALSE) {}
            double gpuMs = std::chrono::duration<double, std::milli>(Clock::now() - tg).count();

            // 키프레임 요청이 있었으면 이번 프레임을 키프레임으로
            // (최소 간격이 안 지났으면 요청을 들고 있다가 간격이 지난 뒤 처리)
            if (keyframeReady) {
                encoder.RequestKeyframe();
                keyframePending = false;
            }

            // 측정: 인코딩 시간
            bool keyframe = false;
            const double frameSeconds = MediaSecondsNow();   // 소리와 같은 기준 시각 (MediaClock.h)
            auto t0 = Clock::now();
            if (encoder.EncodeFrame(packet, keyframe)) {
                statGpuMs += gpuMs;
                statEncodeMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

                if (m_streamer) m_streamer->SendFrame(packet, frameSeconds);

                // 측정: 캡처 → 패킷 전송 호출 끝까지 (송신 PC 안에서의 총 지연)
                if (freshFrame) {
                    const double ageSendMs = (Qpc100ns() - capture.LastFrameTime100ns()) / 1e4;
                    statAgeFrames++;
                    statAgeStartMs += ageStartMs;
                    statAgeSendMs += ageSendMs;
                    statAgeSendMaxMs = std::max(statAgeSendMaxMs, ageSendMs);
                }

                if (dumping) {
                    dumpFile.write(reinterpret_cast<const char*>(packet.data()), packet.size());
                    if (Clock::now() - dumpStart >= dumpDuration) {
                        dumping = false;
                        dumpFile.close();
                        std::cout << "10초 저장 완료\n";
                    }
                }

                statFrames++;
                statBytes += packet.size();
                if (keyframe) {
                    statKeyframes++;
                    lastKeyframe = Clock::now();
                }
            }
        }

        // 1초마다 통계
        double elapsed = std::chrono::duration<double>(Clock::now() - statsStart).count();
        if (elapsed >= 1.0) {
            std::cout << std::fixed << std::setprecision(1)
                      << "인코딩 " << statFrames / elapsed << "fps | "
                      << statBytes * 8 / elapsed / 1'000'000 << "Mbps | "
                      << "GPU 대기 " << (statFrames ? statGpuMs / statFrames : 0) << "ms | "
                      << "인코딩 " << (statFrames ? statEncodeMs / statFrames : 0) << "ms | "
                      << "캡처→시작 " << (statAgeFrames ? statAgeStartMs / statAgeFrames : 0) << "ms | "
                      << "캡처→전송 " << (statAgeFrames ? statAgeSendMs / statAgeFrames : 0)
                      << "ms (최대 " << statAgeSendMaxMs << ") | "
                      << "키프레임 " << statKeyframes
                      << " | 시청자 " << (m_streamer ? m_streamer->ViewerCount() : 0) << "\n";
            statsStart = Clock::now();
            statFrames = statBytes = statKeyframes = 0;
            statEncodeMs = statGpuMs = 0;
            statAgeFrames = 0;
            statAgeStartMs = statAgeSendMs = statAgeSendMaxMs = 0;
        }
    }
}
