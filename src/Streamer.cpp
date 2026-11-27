// libdatachannel(winsock2.h)을 windows.h보다 먼저 include 해야 한다
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "Streamer.h"
#include "Diag.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <random>

using json = nlohmann::json;

// server.py 프로토콜 (서버는 offer / answer / candidate 를 중계만 함):
//   송신기 -> 서버 : {"type":"join","role":"sender","room":"<방>"}
//   서버 -> 송신기 : {"type":"joined", ...}
//   서버 -> 송신기 : {"type":"viewer-joined","viewer":"<id>"}
//   송신기 -> 서버 : {"type":"offer","to":"<id>","sdp":"..."}
//   서버 -> 송신기 : {"type":"answer","from":"<id>","sdp":"..."}
//   서버 -> 송신기 : {"type":"candidate","from":"<id>","candidate":"...","sdpMid":"..."}
//   서버 -> 송신기 : {"type":"viewer-left","viewer":"<id>"}
//
// 송신기 쪽 ICE 후보는 수집이 끝난 뒤 offer SDP에 모두 담아 한 번에 보낸다.
// 시청자 쪽 후보(trickle)는 받아서 추가한다.

namespace {

struct Viewer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::RtpPacketizationConfig> audioConfig;
    bool remoteSet = false;
    std::vector<rtc::Candidate> pending;
};

uint32_t RandomSsrc() {
    std::random_device rd;
    return std::uniform_int_distribution<uint32_t>(1, 0xFFFFFFF0)(rd);
}

// 진단: 실제로 나가는 RTP 패킷의 순서번호·타임스탬프가 고르게 늘어나는지 검사해서 2초마다 출력
class RtpChecker final : public rtc::MediaHandler {
public:
    RtpChecker(std::string name, uint32_t tsStep) : m_name(std::move(name)), m_step(tsStep) {}

    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (const auto& msg : messages) {
            if (!msg || msg->type != rtc::Message::Binary || msg->size() < 12) continue;
            const uint8_t b1 = std::to_integer<uint8_t>((*msg)[1]);
            if (b1 >= 200 && b1 <= 204) continue;   // RTCP(SR 등)는 건너뜀
            const auto* h = reinterpret_cast<const rtc::RtpHeader*>(msg->data());
            const uint16_t seq = h->seqNumber();
            const uint32_t ts = h->timestamp();
            if (m_has) {
                if (uint16_t(seq - m_lastSeq) != 1) m_seqBad++;
                const int32_t dt = int32_t(ts - m_lastTs);
                if (dt == 0) m_tsSame++;
                else if (dt < 0) m_tsBack++;
                else if (dt != int32_t(m_step)) m_tsOdd++;
                m_minDt = std::min(m_minDt, dt);
                m_maxDt = std::max(m_maxDt, dt);
            }
            m_has = true;
            m_lastSeq = seq;
            m_lastTs = ts;
            m_count++;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - m_statStart >= std::chrono::seconds(2)) {
            std::cout << "[" << m_name << "] RTP " << m_count << "개 | 순서번호 이상 " << m_seqBad
                      << " | 타임스탬프 같음 " << m_tsSame << " 뒤로 " << m_tsBack << " " << m_step
                      << "아님 " << m_tsOdd << " | 간격 " << (m_count > 1 ? m_minDt : 0) << "~"
                      << (m_count > 1 ? m_maxDt : 0) << "\n";
            m_statStart = now;
            m_count = m_seqBad = m_tsSame = m_tsBack = m_tsOdd = 0;
            m_minDt = INT32_MAX;
            m_maxDt = INT32_MIN;
        }
    }

private:
    std::string m_name;
    uint32_t m_step;
    bool m_has = false;
    uint16_t m_lastSeq = 0;
    uint32_t m_lastTs = 0;
    uint64_t m_count = 0, m_seqBad = 0, m_tsSame = 0, m_tsBack = 0, m_tsOdd = 0;
    int32_t m_minDt = INT32_MAX, m_maxDt = INT32_MIN;
    std::chrono::steady_clock::time_point m_statStart = std::chrono::steady_clock::now();
};

void SendJson(const std::weak_ptr<rtc::WebSocket>& wws, const json& msg) {
    if (auto ws = wws.lock(); ws && ws->isOpen()) ws->send(msg.dump());
}

} // namespace

struct Streamer::Impl {
    std::shared_ptr<rtc::WebSocket> ws;
    std::string url;
    std::string room;
    unsigned maxKbps = 20000;

    mutable std::mutex mutex;
    std::map<std::string, Viewer> viewers;
    std::atomic<bool> keyframeRequested{ false };

    Viewer CreateViewer(const std::string& id);
    void OnMessage(const std::string& text);
};

Viewer Streamer::Impl::CreateViewer(const std::string& id) {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    Viewer v;
    v.pc = std::make_shared<rtc::PeerConnection>(config);

    v.pc->onStateChange([id](rtc::PeerConnection::State state) {
        std::cout << "[" << id << "] 연결 상태: " << state << "\n";
    });

    std::weak_ptr<rtc::PeerConnection> wpc = v.pc;
    std::weak_ptr<rtc::WebSocket> wws = ws;
    v.pc->onGatheringStateChange([id, wpc, wws](rtc::PeerConnection::GatheringState gs) {
        if (gs != rtc::PeerConnection::GatheringState::Complete) return;
        auto pc = wpc.lock();
        if (!pc) return;
        auto desc = pc->localDescription();
        if (!desc) return;
        SendJson(wws, { {"type", "offer"}, {"to", id}, {"sdp", std::string(*desc)} });
    });

    // 송신 전용 H.264 비디오 트랙
    const uint8_t payloadType = 96;
    const uint32_t ssrc = RandomSsrc();
    const std::string cname = "screen";
    // 영상과 소리를 같은 스트림(msid)으로 묶으면 브라우저가 둘의 싱크(립싱크)를 맞춘다.
    // 그런데 소리 버퍼(70~120ms)에 맞추느라 영상까지 그만큼 늦어져서(20ms → 80~90ms),
    // 영상과 소리를 다른 스트림으로 보내 싱크를 끈다. 실험해 보니 어긋남은 느껴지지 않았다.
    const std::string msid = "screen";
    const std::string audioMsid = "screen-audio";

    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(payloadType);
    media.addSSRC(ssrc, cname, msid, "video");
    v.track = v.pc->addTrack(media);

    v.rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    // StartSequence: 3바이트/4바이트 시작 코드 둘 다 인식
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, v.rtpConfig);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(v.rtpConfig));
    // 재전송용으로 최근 패킷을 보관 (기본 512개).
    // libdatachannel의 SRTP 윈도우가 1024라서, 그보다 오래된 패킷은 재전송하려 해도
    // libsrtp가 거부한다 (SRTP protect error, status=10). 1024를 넘기지 말 것.
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(1024));
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([this, id] {
        std::cout << "[" << id << "] 키프레임 요청(PLI)\n";
        keyframeRequested = true;
    }));
    // 페이싱: 한 프레임의 패킷을 한꺼번에 쏟지 않고 1ms 단위로 나눠 보낸다.
    // 와이파이/공유기 버퍼가 순간적으로 넘쳐서 패킷이 버려지는 걸 줄인다.
    // 속도는 상한의 2배로 잡아서 평소 프레임은 지연 없이 나가고, 큰 키프레임만 조금 퍼진다.
    packetizer->addToChain(std::make_shared<rtc::PacingHandler>(
        double(maxKbps) * 1000.0 * 2.0, std::chrono::milliseconds(1)));
    v.track->setMediaHandler(packetizer);

    // 트랙이 열리면 새 시청자가 바로 화면을 볼 수 있게 키프레임을 요청
    v.track->onOpen([this, id] {
        std::cout << "[" << id << "] 영상 전송 시작\n";
        keyframeRequested = true;
    });

    // 송신 전용 Opus 오디오 트랙 (48kHz 스테레오)
    const uint8_t audioPayloadType = 111;
    const uint32_t audioSsrc = RandomSsrc();
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(audioPayloadType,
                       "minptime=10;maxaveragebitrate=128000;stereo=1;sprop-stereo=1;useinbandfec=1");
    audio.addSSRC(audioSsrc, cname, audioMsid, "audio");
    v.audioTrack = v.pc->addTrack(audio);

    v.audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        audioSsrc, cname, audioPayloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(v.audioConfig);
    if (g_diag) audioPacketizer->addToChain(std::make_shared<RtpChecker>(id + " 소리", 480));   // 진단 모드에서만
    // SR(송신 보고)에 RTP 시각 ↔ 실제 시각 대응이 들어가서, 브라우저가 이걸로 영상과 소리 싱크를 맞춘다
    audioPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(v.audioConfig));
    v.audioTrack->setMediaHandler(audioPacketizer);

    v.pc->setLocalDescription();
    return v;
}

void Streamer::Impl::OnMessage(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    }
    catch (const std::exception& e) {
        std::cout << "시그널링: 잘못된 JSON (" << e.what() << ")\n";
        return;
    }

    const std::string type = msg.value("type", "");
    std::lock_guard lock(mutex);

    if (type == "joined") {
        std::string base = url;
        if (base.rfind("ws", 0) == 0) base = "http" + base.substr(2);
        if (auto pos = base.rfind("/ws"); pos != std::string::npos) base = base.substr(0, pos);
        std::cout << "방 \"" << room << "\" 송신 시작 — 시청 주소: " << base << "/view/" << room << "\n";
    }
    else if (type == "viewer-joined") {
        const std::string id = msg.value("viewer", "");
        std::cout << "시청자 입장: " << id << "\n";
        viewers[id] = CreateViewer(id);
    }
    else if (type == "answer") {
        auto it = viewers.find(msg.value("from", ""));
        if (it == viewers.end()) return;
        Viewer& v = it->second;
        v.pc->setRemoteDescription(rtc::Description(msg.value("sdp", ""), "answer"));
        v.remoteSet = true;
        for (auto& c : v.pending) v.pc->addRemoteCandidate(c);
        v.pending.clear();
    }
    else if (type == "candidate") {
        auto it = viewers.find(msg.value("from", ""));
        if (it == viewers.end()) return;
        const std::string cand = msg.value("candidate", "");
        if (cand.empty()) return;
        const std::string mid = msg.contains("sdpMid") && msg["sdpMid"].is_string()
                                    ? msg["sdpMid"].get<std::string>() : "0";
        Viewer& v = it->second;
        try {
            rtc::Candidate c(cand, mid);
            if (v.remoteSet) v.pc->addRemoteCandidate(c);
            else v.pending.push_back(c);
        }
        catch (const std::exception& e) {
            std::cout << "ICE 후보 추가 실패: " << e.what() << "\n";
        }
    }
    else if (type == "viewer-left") {
        const std::string id = msg.value("viewer", "");
        viewers.erase(id);
        std::cout << "시청자 퇴장: " << id << "\n";
    }
    else if (type == "error") {
        std::cout << "시그널링 서버 오류: " << msg.value("message", "") << "\n";
    }
}

Streamer::Streamer() : m(std::make_unique<Impl>()) {}

Streamer::~Streamer() { Stop(); }

bool Streamer::Start(const std::string& url, const std::string& room, unsigned maxKbps) {
    Stop();
    rtc::InitLogger(rtc::LogLevel::Warning);

    m->url = url;
    m->room = room;
    m->maxKbps = maxKbps;
    m->ws = std::make_shared<rtc::WebSocket>();
    std::weak_ptr<rtc::WebSocket> wws = m->ws;
    Impl* impl = m.get();

    m->ws->onOpen([wws, impl] {
        std::cout << "시그널링 서버 연결됨: " << impl->url << "\n";
        SendJson(wws, { {"type", "join"}, {"role", "sender"}, {"room", impl->room} });
    });
    m->ws->onClosed([] { std::cout << "시그널링 서버 연결 끊김\n"; });
    m->ws->onError([](const std::string& e) { std::cout << "시그널링 오류: " << e << "\n"; });
    m->ws->onMessage([impl](auto data) {
        if (std::holds_alternative<std::string>(data)) impl->OnMessage(std::get<std::string>(data));
    });

    try {
        m->ws->open(url);
    }
    catch (const std::exception& e) {
        std::cout << "시그널링 접속 실패: " << e.what() << "\n";
        return false;
    }
    return true;
}

void Streamer::Stop() {
    {
        std::lock_guard lock(m->mutex);
        m->viewers.clear();
    }
    if (m->ws) {
        m->ws->close();
        m->ws.reset();
    }
}

void Streamer::SendFrame(const std::vector<uint8_t>& data, double seconds) {
    if (data.empty()) return;
    const auto* bytes = reinterpret_cast<const std::byte*>(data.data());

    std::lock_guard lock(m->mutex);
    for (auto& [id, v] : m->viewers) {
        if (!v.track || !v.track->isOpen()) continue;
        // 이 프레임의 RTP 타임스탬프(90kHz)를 정한 뒤 보내면 packetizer가 그 값으로 패킷을 만든다
        v.rtpConfig->timestamp = v.rtpConfig->startTimestamp + v.rtpConfig->secondsToTimestamp(seconds);
        try {
            v.track->send(bytes, data.size());
        }
        catch (const std::exception& e) {
            std::cout << "[" << id << "] 전송 실패: " << e.what() << "\n";
        }
    }
}

void Streamer::SendAudio(const uint8_t* data, size_t size, int64_t sampleTime) {
    if (!data || size == 0) return;
    const auto* bytes = reinterpret_cast<const std::byte*>(data);

    std::lock_guard lock(m->mutex);
    for (auto& [id, v] : m->viewers) {
        if (!v.audioTrack || !v.audioTrack->isOpen()) continue;
        // 초 → 타임스탬프 변환을 거치지 않고 샘플 수를 그대로 쓴다 (Opus RTP 시계 = 48kHz).
        // 공유 설정값(rtpConfig->timestamp)에 미리 써 두지 않고, 패킷마다 타임스탬프를 같이 넘긴다
        const uint32_t ts = v.audioConfig->startTimestamp + uint32_t(sampleTime);
        try {
            v.audioTrack->sendFrame(bytes, size, rtc::FrameInfo(ts));
        }
        catch (const std::exception& e) {
            std::cout << "[" << id << "] 소리 전송 실패: " << e.what() << "\n";
        }
    }
}

bool Streamer::ConsumeKeyframeRequest() {
    return m->keyframeRequested.exchange(false);
}

size_t Streamer::ViewerCount() const {
    std::lock_guard lock(m->mutex);
    return m->viewers.size();
}
