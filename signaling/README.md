# 화면공유 시그널링 서버 + 뷰어

WebRTC 시그널링 서버(Python, aiohttp)와 브라우저 뷰어 페이지.
영상은 송신자 → 시청자로 P2P로 가고, 서버는 연결 정보(offer/answer/candidate)만 중계한다.

## 실행

```
pip install -r requirements.txt
python server.py --port 8080
```

- 시청: `http://<서버주소>:8080/view/<방이름>`
- 테스트 송신(브라우저): `http://localhost:8080/send/<방이름>`

테스트 송신 페이지는 C++ 송신기가 완성되기 전에 서버와 뷰어를 확인하기 위한 용도다.
화면 캡처(getDisplayMedia)는 보안 컨텍스트에서만 되므로 송신 페이지는 `localhost`로 열어야 한다.
뷰어 페이지는 같은 네트워크의 다른 PC에서 `http://<내 IP>:8080/view/<방이름>`으로 열어도 된다.

## 구조

```
server.py               시그널링 서버 (WebSocket /ws + 페이지 서빙)
static/viewer.html      뷰어 (수신 전용, 통계 표시)
static/sender_test.html 브라우저 테스트 송신기
```

- 방 하나에 송신자 1명, 시청자 여러 명
- 송신자는 시청자마다 PeerConnection을 하나씩 만든다 (1:N)
- offer는 항상 송신자가 만든다

## 메시지 규격 (C++ 송신기 구현용)

모든 메시지는 WebSocket 텍스트 프레임의 JSON 객체.

### 입장
```json
→ {"type": "join", "role": "sender", "room": "test"}
← {"type": "joined", "id": "be0809b1", "role": "sender", "room": "test"}
```
이미 송신자가 있는 방이면 `{"type":"error",...}` 후 연결이 닫힌다.

### 서버 → 송신자
| 메시지 | 의미 | 송신자가 할 일 |
|---|---|---|
| `{"type":"viewer-joined","viewer":"<id>"}` | 시청자 입장 (먼저 와 있던 시청자도 송신자 입장 시 하나씩 옴) | PeerConnection 생성 → 트랙 추가 → offer 전송 |
| `{"type":"viewer-left","viewer":"<id>"}` | 시청자 퇴장 | 해당 PeerConnection 닫기 |
| `{"type":"answer","from":"<id>","sdp":"..."}` | 시청자의 answer | setRemoteDescription |
| `{"type":"candidate","from":"<id>","candidate":"...","sdpMid":"0"}` | 시청자의 ICE 후보 | addRemoteCandidate |

### 송신자 → 서버 (`to`로 받을 시청자 지정)
```json
{"type": "offer", "to": "<viewer id>", "sdp": "v=0..."}
{"type": "candidate", "to": "<viewer id>", "candidate": "candidate:...", "sdpMid": "0"}
```

### 서버 → 시청자
`joined`(`senderOnline` 포함), `sender-joined`, `sender-left`, `offer`, `candidate`, `error`

시청자는 `to` 없이 보내며, 서버가 항상 그 방 송신자에게 전달한다.

## 참고

- 같은 공유기 안에서는 STUN만으로 연결된다. 외부 인터넷 시청자는 환경에 따라 TURN 서버가 필요할 수 있다.
- 뷰어는 자동재생 정책 때문에 음소거로 시작한다. "소리 켜기" 버튼으로 해제.
- 뷰어의 "통계" 버튼으로 해상도, fps, 비트레이트, 코덱, 손실 패킷을 볼 수 있다.
