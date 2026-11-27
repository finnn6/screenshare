#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 시그널링 서버(signaling/server.py)에 송신자로 접속해서
// 들어오는 시청자마다 WebRTC 연결을 만들고, 인코딩된 H.264 프레임을 모두에게 보낸다.
//
// libdatachannel 헤더는 Streamer.cpp 안에서만 include 한다.
// (windows.h와 winsock2.h 순서 충돌을 피하려고 구현을 숨김)
class Streamer {
public:
    Streamer();
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // url 예: ws://127.0.0.1:8765/ws
    // maxKbps: 인코더 비트레이트 상한. 패킷을 나눠 보내는 속도(페이싱)를 정하는 데 쓴다.
    bool Start(const std::string& url, const std::string& room, unsigned maxKbps = 20000);
    void Stop();

    // 인코딩 스레드에서 호출. data는 Annex-B(00 00 00 01 시작 코드) H.264 한 프레임.
    // seconds는 스트림 시작 기준 시각(초).
    void SendFrame(const std::vector<uint8_t>& data, double seconds);

    // 오디오 스레드에서 호출. data는 Opus 패킷 하나(10ms).
    // sampleTime: 첫 샘플의 시각을 48kHz 샘플 수로 센 값 (MediaClock 기준점부터). 패킷마다 정확히 480씩 늘어난다.
    void SendAudio(const uint8_t* data, size_t size, int64_t sampleTime);

    // 새 시청자가 들어왔거나 시청자가 PLI(키프레임 요청)를 보냈으면 true를 한 번 돌려준다.
    bool ConsumeKeyframeRequest();

    size_t ViewerCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
