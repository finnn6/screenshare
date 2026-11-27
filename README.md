# screenshare

Windows 화면(모니터 또는 창)과 소리를 캡처해서, 설치 없이 브라우저로 볼 수 있게 보내는 화면공유 프로그램입니다.
기존 화면공유 도구보다 높은 화질과 프레임으로 게임, 유튜브 화면을 공유해 보려고 만들었습니다.
C++와 Windows 그래픽 API를 공부하는 것도 목적입니다.

## 구성

```
[송신 PC: screenshare.exe (C++)]
  화면 캡처 (Windows Graphics Capture)
    → 크기 조절 (D3D11)
    → H.264 인코딩 (NVENC)        ─┐
  소리 캡처 (WASAPI 루프백)         ├→ WebRTC (libdatachannel) ──→ 브라우저 뷰어
    → Opus 인코딩                  ─┘          ↑
                                               │ 연결 정보(offer/answer/ICE) 중계
                                  [시그널링 서버 (Python, aiohttp)]
```

- 영상·소리는 송신 PC에서 시청자에게 직접(P2P) 갑니다. 서버는 연결 정보만 중계합니다.
- 시청자마다 WebRTC 연결을 하나씩 만듭니다 (1:N).
- 캡처·인코딩은 미리보기 창과 분리된 전용 스레드와 전용 D3D11 장치에서 돌립니다. GPU 텍스처를 CPU로 복사하지 않고 NVENC에 바로 넘깁니다.
- 창을 공유하면 그 프로그램(하위 프로세스 포함)의 소리만, 모니터를 공유하면 PC 전체 소리를 보냅니다.

| 경로 | 내용 |
|---|---|
| `src/Capture.*`, `CaptureSources.*` | 모니터/창 캡처, 공유 대상 목록 |
| `src/Encoder.*` | NVENC H.264 인코더 (B프레임 없음, 저지연 설정) |
| `src/Pipeline.*` | 캡처 → 크기 조절 → 인코딩 → 전송 스레드, 구간별 지연 통계 |
| `src/AudioCapture.*` | 소리 루프백 캡처 → Opus 인코딩 |
| `src/Streamer.*` | 시그널링 접속, 시청자별 WebRTC 연결, RTP 전송 |
| `signaling/server.py` | 시그널링 서버 |
| `signaling/static/viewer.html` | 브라우저 뷰어 (통계 오버레이 포함) |

시그널링 메시지 규격은 [signaling/README.md](signaling/README.md)에 있습니다.

## 필요 환경

- Windows 10/11 (창별 소리 캡처를 지원하지 않는 버전에서는 PC 전체 소리로 대신 보냄)
- NVIDIA GPU (NVENC)
- Visual Studio 2022 (C++), CMake 3.21 이상, [vcpkg](https://github.com/microsoft/vcpkg)
- Python 3.10 이상

## 빌드와 실행

### 송신기 빌드
```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg 경로>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```
의존 라이브러리(libdatachannel, nlohmann-json, opus)는 `vcpkg.json`에 따라 자동으로 설치됩니다.

### 시그널링 서버
```
cd signaling
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe server.py --port 8765
```

### 송신
```
.\build\Release\screenshare.exe [방이름] [시그널링 주소] [최대 Mbps] [--diag]
```
- 기본값: `test`, `ws://127.0.0.1:8765/ws`, `20`
- 실행하면 공유할 모니터/창 번호를 고릅니다.
- `--diag`: 진단 모드. 처음 10초의 영상(`test.h264`)과 소리(`audio_test.wav`)를 저장하고, 소리 통계와 RTP 검사 결과를 출력합니다.

### 시청
브라우저에서 `http://<서버 주소>:8765/view/<방이름>`을 엽니다.
- 자동재생 정책 때문에 음소거로 시작합니다. "소리 켜기"를 눌러야 들립니다.
- 주소에 `?stats`를 붙이면 통계(fps, 비트레이트, 손실, 지터버퍼, 디코드 시간, RTT 등)를 바로 표시합니다.

다른 PC에서 보려면 Windows 방화벽에서 서버 포트(TCP)와 `screenshare.exe`를 허용해야 합니다.
외부 인터넷에서 보려면 공유기에서 서버 포트를 포트포워딩해야 합니다.

## 해결한 문제

### 1. 와이파이 시청자의 반복 끊김
- **증상**: 와이파이로 보는 맥북에서 화면이 주기적으로 멈춤
- **원인**: 큰 키프레임이 한 번에 나가면서 패킷 손실 → 뷰어가 키프레임 요청(PLI) → 다시 큰 키프레임 → 손실이 반복됨
- **해결**: 비트레이트 상한(20Mbps)과 VBV 버퍼(0.1초) 설정, 키프레임 최소 간격 1초, 패킷을 1ms 단위로 나눠 보내는 페이싱 추가, 재전송(NACK) 버퍼 확대
- **결과**: 약 15Mbps, 120fps 이상에서 키프레임 요청 없이 멈춤 없이 재생

### 2. 소리 지직거림과 소리 지터버퍼 급증
- **증상**: 뷰어 소리가 지직거리고, 소리 지터버퍼가 100~1800ms를 오르내림
- **확인**: 실제로 나가는 RTP 패킷을 검사하는 진단 코드를 넣어 보니, 10ms(480샘플)여야 할 타임스탬프 간격이 448~513으로 흔들림
- **원인**: 캡처 버퍼가 빌 때마다 장치 시각으로 타임스탬프를 다시 맞추면서, 시각 측정의 흔들림이 패킷마다 들어감
- **해결**: 타임스탬프를 48kHz 샘플 수로 세고, 30ms 넘게 어긋날 때만 다시 맞춤
- **결과**: 타임스탬프 간격 480으로 일정, 지직거림 해소

캡처 단계의 잡음은 별도 문제였습니다. 루프백을 자동 형식 변환 옵션으로 열면 캡처한 소리에 잡음이 섞여서, 장치 기본 형식 그대로 받고 직접 변환하도록 바꿨습니다.

### 3. 시청자 2명 접속 시 `SRTP protect error, status=10`
- **확인**: 사용 중인 libdatachannel(0.24.6)과 libsrtp(2.8.1) 소스를 확인
- **원인**: libdatachannel의 SRTP 재생 방지 윈도우는 1024패킷인데, 재전송 버퍼를 4096개로 잡아 두어서 1024개보다 오래된 패킷을 재전송하려 하면 libsrtp가 거부(replay_old)
- **해결**: 재전송 버퍼를 1024개로 맞춤 → 경고 사라짐

### 4. 영상 지연을 늘리던 립싱크
- 영상과 소리를 같은 스트림으로 묶으면 브라우저가 소리 버퍼(70~120ms)에 맞추느라 영상 지터버퍼도 20ms → 80~90ms로 늘어남
- 영상과 소리를 다른 스트림으로 보내 싱크를 끔. 시청 중 어긋남은 느끼지 못함

## 측정 결과

같은 공유기 안, 송신 RTX 2060 Super / 뷰어 맥북 Chrome(와이파이) 기준입니다.

| 구간 | 값 |
|---|---|
| 인코딩 | 약 3ms/프레임 |
| 송신 PC 처리 (프레임을 가져온 뒤 전송까지) | 약 4ms |
| 뷰어 지터버퍼 | 20~40ms |
| 뷰어 디코드 | 약 3.5ms |
| RTT | 3~8ms |
| 프레임 (게임 화면) | 120~150fps |

구간별 값을 더하면 약 30~35ms입니다. 모니터 표시 지연은 빠져 있고, 화면 촬영으로 종단 간 지연을 잰 값은 아닙니다.

## 알려진 한계

- NVIDIA GPU가 있는 Windows에서만 송신할 수 있습니다.
- 한 PC에서 시청자 2명(맥북 탭 2개)을 열면 화면이 거의 멈추는 문제가 있고, 원인을 아직 찾지 못했습니다. 뷰어는 한 브라우저에서 한 탭만 연결하도록 막아 두었습니다. 서로 다른 기기 여러 대로는 충분히 시험하지 않았습니다.
- TURN 서버가 없어서 네트워크 환경에 따라 외부 시청자가 연결되지 않을 수 있습니다.
- 시그널링 서버에 방 비밀번호와 HTTPS가 없습니다. 외부에 열어 둘 때는 주의가 필요합니다.
- 공유 대상은 콘솔에서 번호로 고릅니다.

## 사용한 라이브러리

- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (MPL 2.0): WebRTC
- [Opus](https://opus-codec.org/) (BSD): 소리 인코딩
- [nlohmann/json](https://github.com/nlohmann/json) (MIT): JSON
- [aiohttp](https://github.com/aio-libs/aiohttp) (Apache 2.0): 시그널링 서버
- `third_party/nvenc/nvEncodeAPI.h`: NVIDIA Video Codec SDK 헤더. 파일 안의 NVIDIA 저작권 및 허가 문구를 따릅니다.
