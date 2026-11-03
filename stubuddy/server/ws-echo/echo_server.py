"""Private echo backend for LAN or the loopback-only ECS gateway."""
import argparse
import asyncio
import contextlib
import hmac
import json
import logging
import re
import secrets
from http import HTTPStatus
from pathlib import Path

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from echo_protocol import AUDIO_PARAMS, MAX_MESSAGE, PATH as WS_PATH, ProtocolError, Recording, control, unpack, pack

ROOT = Path(__file__).resolve().parent
LOG = logging.getLogger("stubuddy.echo")


class EchoService:
    def __init__(self, token, *, max_clients=4, echo_delay=0):
        if not re.fullmatch(r"[a-f0-9]{64}", token):
            raise ValueError("Run configure.py to create a valid local token")
        if not 1 <= max_clients <= 16 or not 0 <= echo_delay <= 10:
            raise ValueError("Invalid service limits")
        self.token = token
        self.max_clients = max_clients
        self.echo_delay = echo_delay
        self.connections = set()

    async def authenticate(self, websocket, request):
        if request.path != WS_PATH:
            return websocket.respond(HTTPStatus.NOT_FOUND, "Not found\n")
        try:
            authorization = request.headers.get("Authorization", "")
            device = request.headers.get("Device-Id", "")
            version = request.headers.get("Protocol-Version", "")
        except Exception:
            return websocket.respond(HTTPStatus.BAD_REQUEST, "Invalid headers\n")
        if (not re.fullmatch(r"Bearer [a-f0-9]{64}", authorization) or
                not hmac.compare_digest(authorization, "Bearer " + self.token)):
            return websocket.respond(HTTPStatus.UNAUTHORIZED, "Unauthorized\n")
        if not re.fullmatch(r"[0-9a-fA-F:]{12,17}", device) or version != "1":
            return websocket.respond(HTTPStatus.BAD_REQUEST, "Invalid device headers\n")
        if len(self.connections) >= self.max_clients:
            return websocket.respond(HTTPStatus.SERVICE_UNAVAILABLE, "Service busy\n")
        return None

    async def handle(self, websocket):
        if len(self.connections) >= self.max_clients:
            await websocket.close(1013, "Service busy")
            return
        self.connections.add(websocket)
        session = secrets.token_hex(8)
        current = None
        echo_task = None
        high_turn = 0
        send_lock = asyncio.Lock()

        async def send(message):
            async with send_lock:
                await websocket.send(message if isinstance(message, bytes) else json.dumps(message, separators=(",", ":")))

        async def cancel_echo():
            nonlocal echo_task
            if echo_task:
                echo_task.cancel()
                with contextlib.suppress(asyncio.CancelledError, ConnectionClosed):
                    await echo_task
                echo_task = None

        async def echo(recording, metadata):
            try:
                if self.echo_delay:
                    await asyncio.sleep(self.echo_delay)
                await send({"type": "echo_start", **metadata})
                for sequence, payload in enumerate(recording.packets):
                    await send(pack(recording.turn, sequence, payload))
                    # Yield to listen/abort even when the OS send buffer accepts
                    # the entire recording without awaiting socket backpressure.
                    await asyncio.sleep(0)
                await send({"type": "echo_end", **metadata})
                LOG.info("ECHO OK session=%s turn=%s frames=%s bytes=%s samples=%s crc32=%08x",
                         session, recording.turn, metadata["frames"], metadata["bytes"],
                         metadata["samples"], metadata["crc32"])
            finally:
                recording.clear()

        try:
            hello = control(await asyncio.wait_for(websocket.recv(), 10))
            if (hello.get("type") != "hello" or hello.get("transport") != "websocket" or
                    type(hello.get("echo_protocol")) is not int or hello.get("echo_protocol") != 1 or
                    hello.get("audio_params") != AUDIO_PARAMS or
                    any(type(v) is bool for v in (hello.get("audio_params") or {}).values())):
                raise ProtocolError("incompatible hello")
            await send({"type": "hello", "transport": "websocket", "echo_protocol": 1,
                        "session_id": session, "audio_params": AUDIO_PARAMS})
            LOG.info("READY session=%s", session)
            while True:
                raw = await asyncio.wait_for(websocket.recv(), 90)
                if isinstance(raw, bytes):
                    turn, sequence, payload = unpack(raw)
                    if current is None or turn != current.turn:
                        if turn <= high_turn:
                            continue
                        raise ProtocolError("audio without start")
                    current.append(sequence, payload)
                    payload = None
                    continue
                message = control(raw)
                kind = message["type"]
                if kind == "ping":
                    await send({"type": "pong"})
                    continue
                turn = message.get("turn")
                if type(turn) is not int or not 1 <= turn <= 0xFFFFFFFF:
                    raise ProtocolError("invalid turn")
                if kind == "listen" and message.get("state") == "start":
                    if turn <= high_turn:
                        raise ProtocolError("turn must increase")
                    await cancel_echo()
                    if current:
                        current.clear()
                    current = Recording(turn)
                    high_turn = turn
                elif kind == "abort":
                    if turn == high_turn:
                        await cancel_echo()
                        if current:
                            current.clear()
                            current = None
                        LOG.info("ABORT session=%s turn=%s", session, turn)
                elif kind == "listen" and message.get("state") == "stop":
                    if current is None or turn != current.turn:
                        if turn < high_turn:
                            continue
                        raise ProtocolError("stop without start")
                    metadata = current.finish(message)
                    await cancel_echo()
                    echo_task = asyncio.create_task(echo(current, metadata))
                    current = None
                else:
                    raise ProtocolError("unknown control")
        except (ProtocolError, asyncio.TimeoutError):
            LOG.warning("PROTOCOL REJECT session=%s", session)
            await websocket.close(1008, "Invalid echo protocol or timeout")
        except ConnectionClosed:
            pass
        finally:
            await cancel_echo()
            if current:
                current.clear()
            self.connections.discard(websocket)
            LOG.info("CLOSED session=%s; audio buffers released", session)

    def server(self, host="0.0.0.0", port=8765):
        return serve(self.handle, host, port, process_request=self.authenticate,
                     max_size=MAX_MESSAGE, max_queue=8, write_limit=16384,
                     compression=None, open_timeout=10, close_timeout=2,
                     ping_interval=10, ping_timeout=10)


async def main(args):
    try:
        config = json.loads(args.config.read_text(encoding="utf-8"))
        service = EchoService(config["token"], echo_delay=args.echo_delay)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit("Run configure.py before starting the echo service") from error
    async with service.server(args.host, config["port"]):
        LOG.info("LISTEN %s:%s%s; Opus 16000Hz/mono/60ms; authentication enabled",
                 args.host, config["port"], WS_PATH)
        LOG.info("RAM-only echo; Ctrl+C stops the service")
        await asyncio.Future()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / ".local.json")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--echo-delay", type=float, default=0, help="0..10 seconds, for wait/abort acceptance")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.ERROR)
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass
