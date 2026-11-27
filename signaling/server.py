"""화면공유 시그널링 서버

- WebSocket(/ws)으로 송신자(sender) 1명과 시청자(viewer) 여러 명을 방(room) 단위로 연결한다.
- 서버는 offer / answer / candidate 메시지를 상대에게 그대로 전달만 한다. 영상은 서버를 거치지 않는다.
- 뷰어 페이지(/view/<room>)와 브라우저 테스트 송신 페이지(/send/<room>)도 같이 서빙한다.

실행: python server.py --port 8080
"""

import argparse
import json
import logging
import secrets
from pathlib import Path

from aiohttp import WSMsgType, web

STATIC_DIR = Path(__file__).parent / "static"
RELAY_TYPES = {"offer", "answer", "candidate"}

log = logging.getLogger("signaling")


class Peer:
    def __init__(self, ws: web.WebSocketResponse, role: str, room: "Room"):
        self.id = secrets.token_hex(4)
        self.ws = ws
        self.role = role
        self.room = room

    async def send(self, msg: dict) -> None:
        if self.ws.closed:
            return
        try:
            await self.ws.send_json(msg)
        except (ConnectionResetError, RuntimeError):
            pass


class Room:
    def __init__(self, name: str):
        self.name = name
        self.sender: Peer | None = None
        self.viewers: dict[str, Peer] = {}

    def is_empty(self) -> bool:
        return self.sender is None and not self.viewers


def get_room(app: web.Application, name: str) -> Room:
    rooms: dict[str, Room] = app["rooms"]
    if name not in rooms:
        rooms[name] = Room(name)
    return rooms[name]


async def send_error(ws: web.WebSocketResponse, message: str) -> None:
    if not ws.closed:
        await ws.send_json({"type": "error", "message": message})


async def join(app: web.Application, ws: web.WebSocketResponse, data: dict) -> Peer | None:
    role = data.get("role")
    room_name = data.get("room")

    if role not in ("sender", "viewer"):
        await send_error(ws, "role은 sender 또는 viewer여야 합니다")
        return None
    if not isinstance(room_name, str) or not (1 <= len(room_name) <= 64):
        await send_error(ws, "room은 1~64자 문자열이어야 합니다")
        return None

    room = get_room(app, room_name)

    if role == "sender":
        if room.sender is not None:
            await send_error(ws, "이 방에는 이미 송신자가 있습니다")
            return None
        peer = Peer(ws, role, room)
        room.sender = peer
        await peer.send({"type": "joined", "id": peer.id, "role": role, "room": room_name})
        # 먼저 들어와 기다리던 시청자들을 송신자에게 알려준다
        for viewer in room.viewers.values():
            await peer.send({"type": "viewer-joined", "viewer": viewer.id})
            await viewer.send({"type": "sender-joined"})
    else:
        peer = Peer(ws, role, room)
        room.viewers[peer.id] = peer
        await peer.send({
            "type": "joined",
            "id": peer.id,
            "role": role,
            "room": room_name,
            "senderOnline": room.sender is not None,
        })
        if room.sender is not None:
            await room.sender.send({"type": "viewer-joined", "viewer": peer.id})

    log.info("join  room=%s role=%s id=%s (viewers=%d)", room_name, role, peer.id, len(room.viewers))
    return peer


async def relay(peer: Peer, data: dict) -> None:
    msg_type = data.get("type")
    if msg_type not in RELAY_TYPES:
        await send_error(peer.ws, f"알 수 없는 메시지 타입: {msg_type}")
        return

    room = peer.room
    if peer.role == "viewer":
        # 시청자는 송신자에게만 보낼 수 있다
        target = room.sender
    else:
        target = room.viewers.get(data.get("to", ""))

    if target is None:
        await send_error(peer.ws, "받을 상대가 없습니다")
        return

    out = {k: v for k, v in data.items() if k != "to"}
    out["from"] = peer.id
    await target.send(out)


async def leave(app: web.Application, peer: Peer) -> None:
    room = peer.room
    if peer.role == "sender":
        if room.sender is peer:
            room.sender = None
            for viewer in room.viewers.values():
                await viewer.send({"type": "sender-left"})
    else:
        room.viewers.pop(peer.id, None)
        if room.sender is not None:
            await room.sender.send({"type": "viewer-left", "viewer": peer.id})

    if room.is_empty():
        app["rooms"].pop(room.name, None)

    log.info("leave room=%s role=%s id=%s (viewers=%d)", room.name, peer.role, peer.id, len(room.viewers))


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    app = request.app
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)

    peer: Peer | None = None
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            try:
                data = json.loads(msg.data)
                if not isinstance(data, dict):
                    raise ValueError
            except ValueError:
                await send_error(ws, "JSON 객체 형식이 아닙니다")
                continue

            if peer is None:
                if data.get("type") != "join":
                    await send_error(ws, "먼저 join 메시지를 보내야 합니다")
                    continue
                peer = await join(app, ws, data)
                if peer is None:
                    await ws.close()
                    break
                continue

            await relay(peer, data)
    finally:
        if peer is not None:
            await leave(app, peer)

    return ws


async def index(request: web.Request) -> web.Response:
    return web.Response(
        text="화면공유 시그널링 서버\n\n시청: /view/<방이름>\n테스트 송신(브라우저): /send/<방이름>\n",
        content_type="text/plain",
    )


async def viewer_page(request: web.Request) -> web.FileResponse:
    return web.FileResponse(STATIC_DIR / "viewer.html")


async def sender_page(request: web.Request) -> web.FileResponse:
    return web.FileResponse(STATIC_DIR / "sender_test.html")


def create_app() -> web.Application:
    app = web.Application()
    app["rooms"] = {}
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/view/{room}", viewer_page)
    app.router.add_get("/send/{room}", sender_page)
    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="화면공유 시그널링 서버")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    web.run_app(create_app(), host=args.host, port=args.port)


if __name__ == "__main__":
    main()
