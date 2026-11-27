#include "AudioCapture.h"
#include "MediaClock.h"
#include "Diag.h"
#include "Streamer.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <audioclientactivationparams.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kRate = 48000;                 // Opus/WebRTC 표준
constexpr int kChannels = 2;
constexpr int kFrameSamples = kRate / 100;   // 10ms 단위로 인코딩 (지연을 줄이려고 20ms 대신)
constexpr int kBitrate = 128000;             // 게임/음악용 스테레오
constexpr double kResyncSec = 0.03;          // 패킷 시각이 예상과 이만큼 어긋나면 다시 맞춤

// 진단용 WAV (16비트 PCM 스테레오). dataBytes를 나중에 채우려고 헤더를 다시 쓸 수 있게 함
void WriteWavHeader(std::ofstream& f, uint32_t dataBytes) {
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.seekp(0);
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(kChannels); u32(kRate);
    u32(kRate * kChannels * 2); u16(kChannels * 2); u16(16);
    f.write("data", 4); u32(dataBytes);
}
bool g_wavDone = false;   // 장치가 바뀌어 다시 연결돼도 한 번만 저장

// ActivateAudioInterfaceAsync는 결과를 콜백으로 알려준다 → 끝날 때까지 기다리려고 이벤트로 받음
// (FtmBase: 이 객체를 다른 스레드에서 불러도 되게 함. 이 API가 요구함)
class ActivateHandler : public Microsoft::WRL::RuntimeClass<
                            Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                            Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    ActivateHandler() { m_done = CreateEvent(nullptr, TRUE, FALSE, nullptr); }
    ~ActivateHandler() override { if (m_done) CloseHandle(m_done); }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activateHr = E_FAIL;
        ComPtr<IUnknown> unk;
        HRESULT hr = op->GetActivateResult(&activateHr, &unk);
        if (SUCCEEDED(hr)) hr = activateHr;
        if (SUCCEEDED(hr)) hr = unk.As(&m_client);
        m_hr = hr;
        SetEvent(m_done);
        return S_OK;
    }

    HANDLE m_done = nullptr;
    HRESULT m_hr = E_FAIL;
    ComPtr<IAudioClient> m_client;
};

// 프로그램 하나(+하위 프로세스)의 소리만 내보내는 가상 장치를 연다.
// 크롬처럼 소리를 하위 프로세스에서 내는 프로그램도 잡히도록 INCLUDE_TARGET_PROCESS_TREE 사용
HRESULT ActivateProcessLoopback(DWORD pid, ComPtr<IAudioClient>& out) {
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Microsoft::WRL::Make<ActivateHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &pv, handler.Get(), &op);
    if (FAILED(hr)) return hr;
    if (WaitForSingleObject(handler->m_done, 5000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (FAILED(handler->m_hr)) return handler->m_hr;
    out = handler->m_client;
    return S_OK;
}

struct OpusDeleter { void operator()(OpusEncoder* e) const { opus_encoder_destroy(e); } };
}

bool AudioPipeline::Start(Streamer* streamer, DWORD targetPid) {
    Stop();
    m_streamer = streamer;
    m_targetPid = targetPid;
    m_stopRequested = false;
    m_thread = std::thread(&AudioPipeline::ThreadMain, this);
    return true;
}

void AudioPipeline::Stop() {
    m_stopRequested = true;
    if (m_thread.joinable()) m_thread.join();
}

void AudioPipeline::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!m_stopRequested) {
        if (!Run()) break;
        // 장치가 바뀌었거나 실패했으면 잠시 뒤 다시 시도
        for (int i = 0; i < 10 && !m_stopRequested; ++i) Sleep(100);
    }
    CoUninitialize();
}

bool AudioPipeline::Run() {
    HRESULT hr = S_OK;
    ComPtr<IAudioClient> client;
    bool isFloat = true;
    int srcChannels = kChannels;
    int srcBytes = 4;
    std::string sourceName;
    HANDLE ev = nullptr;   // 프로세스 루프백은 새 소리가 오면 이 이벤트로 알려준다

    // 1-a) 공유 대상 프로그램의 소리만
    if (m_targetPid != 0) {
        hr = ActivateProcessLoopback(m_targetPid, client);
        if (FAILED(hr)) {
            std::cout << "프로그램 소리만 캡처하기 실패: 0x" << std::hex << hr << std::dec
                      << " (윈도우 10 2004 이상 필요) — PC 전체 소리로 대신함\n";
            m_targetPid = 0;
            client.Reset();
        }
    }
    if (m_targetPid != 0) {
        // 프로세스 루프백은 장치 형식(GetMixFormat)을 알려주지 않는다 → 48kHz 스테레오 float로 직접 요청
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = kChannels;
        fmt.nSamplesPerSec = kRate;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            // 이 형식을 그대로 못 주면 그때만 자동 변환 옵션을 켠다 (시스템 루프백에서는 이 옵션이 잡음을 냈음)
            std::cout << "프로그램 소리 초기화 실패(0x" << std::hex << hr << std::dec << ") — 자동 변환으로 다시 시도\n";
            client.Reset();
            hr = ActivateProcessLoopback(m_targetPid, client);
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        2'000'000, 0, &fmt, nullptr);
        }
        if (FAILED(hr)) { std::cout << "프로그램 소리 캡처 초기화 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }

        ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(ev);
        if (FAILED(hr)) { std::cout << "소리 이벤트 설정 실패: 0x" << std::hex << hr << std::dec << "\n"; CloseHandle(ev); return true; }
        sourceName = "공유 프로그램 소리만 (PID " + std::to_string(m_targetPid) + ")";
    }
    // 1-b) 기본 출력 장치로 나가는 소리 전체 (모니터 공유이거나 위가 안 될 때)
    else {
        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { std::cout << "오디오 장치 목록 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) { std::cout << "기본 출력 장치 없음: 0x" << std::hex << hr << std::dec << "\n"; return true; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { std::cout << "오디오 클라이언트 생성 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }

        // 장치의 믹스 형식 그대로 루프백으로 연다 (자동 변환 옵션은 잡음 문제가 있어서 쓰지 않음)
        WAVEFORMATEX* mixRaw = nullptr;
        hr = client->GetMixFormat(&mixRaw);
        if (FAILED(hr) || !mixRaw) { std::cout << "출력 장치 형식 읽기 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(mixRaw, &CoTaskMemFree);

        // 샘플이 float인지 정수인지 (EXTENSIBLE이면 SubFormat GUID의 첫 값이 1=PCM, 3=float)
        isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix.get())->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
        srcChannels = mix->nChannels;
        srcBytes = mix->wBitsPerSample / 8;
        std::cout << "출력 장치 형식: " << mix->nSamplesPerSec << "Hz " << srcChannels << "ch "
                  << mix->wBitsPerSample << "bit " << (isFloat ? "float" : "정수") << "\n";

        if (mix->nSamplesPerSec != (DWORD)kRate) {
            std::cout << "48kHz가 아닌 출력 장치는 아직 지원하지 않음 — 소리 공유 끔 "
                         "(Windows 소리 설정에서 장치 형식을 48000Hz로 바꾸면 됨)\n";
            return false;
        }
        if (isFloat ? srcBytes != 4 : (srcBytes != 2 && srcBytes != 3 && srcBytes != 4)) {
            std::cout << "지원하지 않는 샘플 형식 — 소리 공유 끔\n";
            return false;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                2'000'000 /* 버퍼 200ms (100ns 단위) */, 0, mix.get(), nullptr);
        if (FAILED(hr)) { std::cout << "루프백 캡처 초기화 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }
        sourceName = "PC 전체 소리 (기본 출력 장치)";
    }
    std::unique_ptr<void, decltype(&CloseHandle)> evGuard(ev, &CloseHandle);   // 함수가 끝나면 이벤트 닫기

    // 장치 샘플 하나를 -1~1 float로 읽기
    auto readSample = [&](const BYTE* p) -> float {
        if (isFloat) { float v; memcpy(&v, p, 4); return v; }
        if (srcBytes == 2) { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
        if (srcBytes == 3) { int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); return v / 2147483648.0f; }
        int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f;
    };
    const size_t frameStride = size_t(srcChannels) * srcBytes;

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) { std::cout << "캡처 서비스 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }

    // 3) Opus 인코더
    int err = 0;
    std::unique_ptr<OpusEncoder, OpusDeleter> enc(
        opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &err));
    if (err != OPUS_OK || !enc) { std::cout << "Opus 인코더 생성 실패: " << err << "\n"; return true; }
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc.get(), OPUS_SET_INBAND_FEC(1));       // 손실 대비 (뷰어 쪽 useinbandfec=1)
    opus_encoder_ctl(enc.get(), OPUS_SET_PACKET_LOSS_PERC(5));

    hr = client->Start();
    if (FAILED(hr)) { std::cout << "오디오 캡처 시작 실패: 0x" << std::hex << hr << std::dec << "\n"; return true; }
    std::cout << "소리 공유 시작: " << sourceName << ", Opus " << kRate / 1000 << "kHz 스테레오 "
              << kBitrate / 1000 << "kbps\n";

    // pcm: 아직 인코딩 안 한 샘플 (L R L R ...). bufStartSec: pcm 첫 샘플이 캡처된 시각
    std::vector<float> pcm;
    pcm.reserve(kRate * kChannels);
    double bufStartSec = 0;
    int64_t nextFrameSample = 0;   // 다음에 보낼 패킷 첫 샘플의 시각 (48kHz 샘플 수)
    // 시각이 이어지고 있는지. pcm이 비어 있어도(10ms씩 딱 맞게 다 인코딩한 경우) 시각은 이어진다.
    // 이전에는 pcm이 비면 매번 장치 시각으로 다시 맞춰서, 패킷마다 타임스탬프가 흔들렸다
    bool haveTime = false;
    std::vector<uint8_t> packet(4000);

    // 진단: 캡처한 소리를 처음 10초 저장 (캡처 단계에서 이미 깨졌는지 확인)
    std::ofstream wav;
    uint32_t wavFrames = 0;
    const uint32_t wavMaxFrames = kRate * 10;
    if (g_diag && !g_wavDone) {   // 진단 모드에서만
        wav.open("audio_test.wav", std::ios::binary);
        WriteWavHeader(wav, 0);
        std::cout << "소리 저장 위치: " << std::filesystem::absolute("audio_test.wav").string() << "\n";
    }

    // 진단용 통계 (2초마다 출력)
    auto statStart = Qpc100ns();
    uint64_t statSent = 0, statForward = 0, statBackward = 0, statDiscont = 0, statTsError = 0;
    double statAgeMs = 0, statAgeMaxMs = 0;

    while (!m_stopRequested) {
        if (ev) WaitForSingleObject(ev, 50);   // 프로세스 루프백: 새 소리가 오면 깨어남
        else Sleep(5);   // 시스템 루프백은 이벤트 알림이 불안정해서 5ms마다 확인 (main에서 timeBeginPeriod(1))

        UINT32 next = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpcPos = 0;   // 이 패킷 첫 샘플의 캡처 시각 (QPC, 100ns 단위)
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos);
            if (FAILED(hr)) break;

            const double expected = bufStartSec + double(pcm.size() / kChannels) / kRate;
            double packetSec;
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpcPos == 0) {
                // 장치가 준 시각을 믿을 수 없음 → 이어지는 걸로 보거나, 지금 시각에서 패킷 길이만큼 뺀다
                statTsError++;
                packetSec = haveTime ? expected : MediaSecondsNow() - double(frames) / kRate;
            }
            else {
                packetSec = ToMediaSeconds((int64_t)qpcPos);
            }

            // 아무 소리도 안 나면 윈도우가 패킷을 안 준다 → 다시 소리가 나면 시각이 건너뛴다.
            // 이어지지 않는 패킷이면 남은 조각(10ms 미만)은 버리고 새 시각부터 다시 시작
            if (haveTime) {
                const double diff = packetSec - expected;
                const bool discont = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                if (discont || std::abs(diff) > kResyncSec) {
                    if (discont) statDiscont++;
                    else if (diff > 0) statForward++;
                    else statBackward++;   // 시각이 뒤로 감 → RTP 타임스탬프가 거꾸로 갈 수 있어서 위험
                    pcm.clear();
                    haveTime = false;
                }
            }
            if (!haveTime) {
                haveTime = true;
                bufStartSec = packetSec;
                // 다시 맞출 때도 이미 보낸 구간보다 뒤로 가지 않게 (타임스탬프가 거꾸로 가면 수신 측이 버린다)
                nextFrameSample = std::max<int64_t>(std::llround(packetSec * kRate), nextFrameSample);
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pcm.insert(pcm.end(), size_t(frames) * kChannels, 0.0f);
            }
            else {
                // 장치 형식 → 스테레오 float (채널이 하나면 양쪽에 복사, 셋 이상이면 앞 두 채널)
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE* fp = data + i * frameStride;
                    const float l = readSample(fp);
                    const float r = srcChannels >= 2 ? readSample(fp + srcBytes) : l;
                    pcm.push_back(l);
                    pcm.push_back(r);
                }
            }
            if (wav.is_open()) {
                const float* src = pcm.data() + pcm.size() - size_t(frames) * kChannels;
                for (size_t i = 0; i < size_t(frames) * kChannels; ++i) {
                    const float v = std::clamp(src[i], -1.0f, 1.0f);
                    const int16_t s16 = (int16_t)std::lround(v * 32767.0f);
                    wav.write(reinterpret_cast<const char*>(&s16), 2);
                }
                wavFrames += frames;
                if (wavFrames >= wavMaxFrames) {
                    WriteWavHeader(wav, wavFrames * kChannels * 2);
                    wav.close();
                    g_wavDone = true;
                    std::cout << "소리 10초 저장 완료\n";
                }
            }
            capture->ReleaseBuffer(frames);

            // 10ms씩 잘라서 인코딩 → 전송
            while (pcm.size() >= size_t(kFrameSamples) * kChannels) {
                const int n = opus_encode_float(enc.get(), pcm.data(), kFrameSamples,
                                                packet.data(), (opus_int32)packet.size());
                if (n > 0 && m_streamer && nextFrameSample >= 0) {
                    m_streamer->SendAudio(packet.data(), size_t(n), nextFrameSample);
                    // 진단: 이 패킷 첫 샘플이 캡처된 뒤 얼마 만에 보냈는지
                    const double ageMs = (MediaSecondsNow() - bufStartSec) * 1000.0;
                    statSent++;
                    statAgeMs += ageMs;
                    statAgeMaxMs = std::max(statAgeMaxMs, ageMs);
                }
                pcm.erase(pcm.begin(), pcm.begin() + size_t(kFrameSamples) * kChannels);
                bufStartSec += double(kFrameSamples) / kRate;
                nextFrameSample += kFrameSamples;
            }
        }

        const double statSec = (Qpc100ns() - statStart) / 1e7;
        if (statSec >= 2.0) {
            if (g_diag) std::cout << std::fixed << std::setprecision(1)
                      << "소리 " << statSent / statSec << "패킷/s (정상 100) | 캡처→전송 "
                      << (statSent ? statAgeMs / statSent : 0) << "ms (최대 " << statAgeMaxMs << ") | "
                      << "다시맞춤 앞 " << statForward << " 뒤 " << statBackward
                      << " 끊김 " << statDiscont << " 시각오류 " << statTsError << "\n";
            statStart = Qpc100ns();
            statSent = statForward = statBackward = statDiscont = statTsError = 0;
            statAgeMs = statAgeMaxMs = 0;
        }

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            std::cout << "출력 장치가 바뀜 — 소리 캡처 다시 연결\n";
            break;
        }
        if (FAILED(hr)) {
            std::cout << "오디오 캡처 오류: 0x" << std::hex << hr << std::dec << "\n";
            break;
        }
    }
    client->Stop();
    return true;
}
