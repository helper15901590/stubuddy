"""stubuddy ai/1: cancellable PTT ASR -> LLM -> TTS with an eight-round window."""
import argparse
import asyncio
import contextlib
import hmac
import json
import logging
from pathlib import Path
import re
import secrets
import sys
import time
import zlib
from http import HTTPStatus

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

# Reuse the accepted bounded framing/integrity implementation. Packaging uses
# the same tracked source, not a second implementation of echo/1 framing.
sys.path.append(str(Path(__file__).resolve().parent.parent / "ws-echo"))
from echo_protocol import AUDIO_PARAMS, MAX_MESSAGE, ProtocolError, Recording, control, unpack, pack
from audio import decode, encode, StreamEncoder, REPLY_PACKET_LIMIT
from history import History
from providers import DashScope, PROMPT, ProviderFailure

LOG = logging.getLogger("stubuddy.ai")
PATH = "/stubuddy/ai"


class VoiceService:
    def __init__(self, settings, provider=None, codec=None):
        self.token = settings["device_token"]
        if not re.fullmatch(r"[a-f0-9]{64}", self.token):
            raise ValueError("Invalid private device token")
        self.provider = provider if provider is not None else DashScope(settings)
        self.decode, self.encode = codec if codec is not None else (decode, encode)
        self.prompt = settings.get("prompt", PROMPT)
        if not isinstance(self.prompt, str) or not 1 <= len(self.prompt) <= 4000:
            raise ValueError("Invalid system prompt")
        self.rounds = settings.get("history_rounds", 8)
        self.budget = settings.get("history_bytes", 64000)
        History(self.rounds, self.budget)  # Validate before accepting connections.
        self.histories = {}
        self.connected = set()
        self.slots = asyncio.Semaphore(2)

    async def authenticate(self, socket, request):
        if request.path != PATH:
            return socket.respond(HTTPStatus.NOT_FOUND, "Not found\n")
        try:
            authorization = request.headers.get("Authorization", "")
            device = request.headers.get("Device-Id", "").lower()
            version = request.headers.get("Protocol-Version", "")
        except Exception:
            return socket.respond(HTTPStatus.BAD_REQUEST, "Invalid headers\n")
        if (not re.fullmatch(r"Bearer [a-f0-9]{64}", authorization) or
                not hmac.compare_digest(authorization, "Bearer " + self.token)):
            return socket.respond(HTTPStatus.UNAUTHORIZED, "Unauthorized\n")
        if not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", device) or version != "1":
            return socket.respond(HTTPStatus.BAD_REQUEST, "Invalid device headers\n")
        if len(self.connected) >= 2 or device in self.connected:
            return socket.respond(HTTPStatus.SERVICE_UNAVAILABLE, "Service busy\n")
        return None

    async def handle(self, socket):
        device = socket.request.headers["Device-Id"].lower()
        if len(self.connected) >= 2 or device in self.connected:
            await socket.close(1013, "Service busy")
            return
        self.connected.add(device)
        # Bounded per-device RAM, retained on transport reconnect for 30 minutes.
        now = time.monotonic()
        for key, value in list(self.histories.items()):
            if now - value.touched >= value.idle_seconds:
                del self.histories[key]
        if device not in self.histories:
            if len(self.histories) >= 16:
                oldest = min(self.histories, key=lambda key: self.histories[key].touched)
                del self.histories[oldest]
            self.histories[device] = History(self.rounds, self.budget)
        history = self.histories[device]
        session = secrets.token_hex(8)
        high_turn = 0
        current = None
        task = None
        pending = None
        active_turn = 0
        streaming = False
        send_lock = asyncio.Lock()

        async def send(message, turn=0):
            async with send_lock:
                if turn and active_turn != turn:
                    return
                if not isinstance(message, bytes):
                    message = {**message, "session_id": session}
                    if turn:
                        message["turn"] = turn
                    message = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
                await socket.send(message)

        async def cancel():
            nonlocal task, pending
            pending = None
            if task:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError, ConnectionClosed):
                    await task
                task = None

        async def respond(recording, metadata):
            nonlocal pending
            turn = recording.turn
            stage = "asr"
            try:
                async with asyncio.timeout(90):
                    async with self.slots:
                        await send({"type": "stage", "state": stage}, turn)
                        pcm = await asyncio.to_thread(self.decode, recording.packets, metadata["samples"], metadata["delay"])
                        query = await self.provider.recognize(pcm)
                        del pcm
                        if not query:
                            await send({"type": "turn_empty"}, turn)
                            return
                        if len(query.encode("utf-8")) > 6000:
                            raise ValueError("ASR input exceeds bounded context")
                        # Keep the wire JSON below the firmware's 1024-byte bound.
                        shown = query if len(query) <= 180 else query[:80] + "…" + query[-80:]
                        await send({"type": "stt", "text": shown}, turn)
                        stage = "llm"
                        await send({"type": "stage", "state": stage}, turn)
                        answer = await self.provider.answer(history.messages(self.prompt, query))
                        await send({"type": "reply_text", "text": answer}, turn)
                        stage = "tts"
                        await send({"type": "stage", "state": stage}, turn)
                        if streaming:
                            await stream_reply(turn, query, answer)
                            return
                        pcm = await self.provider.synthesize(answer)
                        # The common metadata has a 300ms lower limit.
                        if len(pcm) < 9600:
                            pcm += bytes(9600 - len(pcm))
                        packets, samples, delay = await asyncio.to_thread(self.encode, pcm)
                        del pcm
                        if active_turn != turn:
                            return
                        crc = 0
                        for packet in packets:
                            crc = zlib.crc32(packet, crc)
                        reply = {"turn": turn, "samples": samples, "delay": delay, "frames": len(packets),
                                 "bytes": sum(map(len, packets)), "crc32": crc}
                        # Independently validate newly generated TTS metadata.
                        validation = Recording(turn)
                        for index, packet in enumerate(packets):
                            validation.append(index, packet)
                        validation.finish(reply)
                        validation.clear()
                        pending = (turn, query, answer)
                        await send({"type": "reply_start", **reply}, turn)
                        for index, packet in enumerate(packets):
                            if active_turn != turn:
                                return
                            await send(pack(turn, index, packet), turn)
                            await asyncio.sleep(0)
                        await send({"type": "reply_end", **reply}, turn)
                        LOG.info("REPLY SENT session=%s turn=%s samples=%s; waiting for playback confirmation",
                                 session, turn, samples)
            except asyncio.CancelledError:
                raise
            except ConnectionClosed:
                pass
            except Exception as error:
                pending = None
                # Exception text can contain upstream secrets; log only its class.
                diagnostic = error.code if isinstance(error, ProviderFailure) else type(error).__name__
                LOG.warning("TURN FAILED session=%s turn=%s stage=%s kind=%s", session, turn, stage, diagnostic)
                await send({"type": "turn_error", "code": stage}, turn)
            finally:
                recording.clear()

        async def stream_reply(turn, query, answer):
            nonlocal pending
            encoder = StreamEncoder()
            reply = {"turn": turn, "samples": 0, "delay": encoder.delay,
                     "frames": 0, "bytes": 0, "crc32": 0}
            started = time.monotonic()
            first_audio = None
            max_packet = 0
            await send({"type": "reply_start", "stream": 1, "delay": encoder.delay,
                        "max_packet_bytes": REPLY_PACKET_LIMIT}, turn)

            async def forward(packets):
                nonlocal first_audio, max_packet
                for packet in packets:
                    if active_turn != turn:
                        raise asyncio.CancelledError
                    index = reply["frames"]
                    await send(pack(turn, index, packet), turn)
                    reply["frames"] += 1
                    reply["bytes"] += len(packet)
                    reply["crc32"] = zlib.crc32(packet, reply["crc32"])
                    max_packet = max(max_packet, len(packet))
                    reply["samples"] = min(encoder.samples, reply["frames"] * 960 - encoder.delay)
                    if reply["frames"] % 5 == 0:
                        # Each cumulative checkpoint validates the next <=300ms
                        # before the device exposes those packets to its decoder.
                        await send({"type": "reply_chunk", **reply}, turn)
                        if first_audio is None:
                            first_audio = time.monotonic()
                            LOG.info("STREAM FIRST session=%s turn=%s tts_ms=%s", session, turn,
                                     round((first_audio - started) * 1000))
                    await asyncio.sleep(0)

            async with contextlib.aclosing(self.provider.synthesize_stream(answer)) as chunks:
                async for pcm in chunks:
                    packets = await asyncio.to_thread(encoder.feed, pcm)
                    await forward(packets)
            await forward(await asyncio.to_thread(encoder.finish))
            reply["samples"] = encoder.samples
            pending = (turn, query, answer)
            await send({"type": "reply_end", **reply}, turn)
            LOG.info("STREAM END session=%s turn=%s samples=%s frames=%s max_packet=%s tts_ms=%s; waiting for playback confirmation",
                     session, turn, reply["samples"], reply["frames"], max_packet,
                     round((time.monotonic() - started) * 1000))

        try:
            hello = control(await asyncio.wait_for(socket.recv(), 10))
            if (hello.get("type") != "hello" or hello.get("transport") != "websocket" or
                    type(hello.get("ai_protocol")) is not int or hello["ai_protocol"] != 1 or
                    hello.get("audio_params") != AUDIO_PARAMS or
                    any(type(v) is bool for v in (hello.get("audio_params") or {}).values())):
                raise ProtocolError("Expected negotiated ai/1")
            if "reply_stream" in hello:
                if (type(hello["reply_stream"]) is not int or hello["reply_stream"] != 1 or
                        type(hello.get("max_reply_packet_bytes")) is not int or
                        hello["max_reply_packet_bytes"] != REPLY_PACKET_LIMIT):
                    raise ProtocolError("Invalid reply stream negotiation")
                streaming = True
            await send({"type": "hello", "transport": "websocket", "ai_protocol": 1,
                        "audio_params": AUDIO_PARAMS, **({"reply_stream": 1,
                        "max_reply_packet_bytes": REPLY_PACKET_LIMIT} if streaming else {})})
            LOG.info("READY session=%s history=%s completed rounds; current input is extra", session, self.rounds)
            while True:
                raw = await asyncio.wait_for(socket.recv(), 90)
                if isinstance(raw, bytes):
                    turn, sequence, payload = unpack(raw)
                    if current is None or current.turn != turn:
                        if turn <= high_turn:
                            continue
                        raise ProtocolError("Audio without start")
                    current.append(sequence, payload)
                    continue
                message = control(raw)
                if message["type"] == "ping":
                    await send({"type": "pong"})
                    continue
                turn = message.get("turn")
                if type(turn) is not int or not 1 <= turn <= 0xFFFFFFFF:
                    raise ProtocolError("Invalid turn")
                kind = message["type"]
                if kind == "listen" and message.get("state") == "start":
                    if turn <= high_turn or message.get("mode") != "manual":
                        raise ProtocolError("Invalid start")
                    active_turn = turn  # Invalidate late output before awaiting cancellation.
                    await cancel()
                    if current:
                        current.clear()
                    current = Recording(turn)
                    high_turn = turn
                elif kind == "abort":
                    if turn == high_turn:
                        active_turn = 0
                        await cancel()
                        if current:
                            current.clear()
                            current = None
                elif kind == "playback_done":
                    if pending and pending[0] == turn and active_turn == turn:
                        history.commit(pending[1], pending[2])
                        pending = None
                        LOG.info("HISTORY COMMIT session=%s turn=%s stored_rounds=%s", session, turn, len(history.pairs))
                elif kind == "listen" and message.get("state") == "stop":
                    if current is None or current.turn != turn:
                        if turn < high_turn:
                            continue
                        raise ProtocolError("Stop without start")
                    metadata = current.finish(message)
                    await cancel()
                    task = asyncio.create_task(respond(current, metadata))
                    current = None
                else:
                    raise ProtocolError("Unknown control")
        except (ProtocolError, asyncio.TimeoutError, ValueError):
            LOG.warning("PROTOCOL REJECT session=%s", session)
            await socket.close(1008, "Invalid ai protocol or timeout")
        except ConnectionClosed:
            pass
        finally:
            active_turn = 0
            await cancel()
            if current:
                current.clear()
            self.connected.discard(device)
            LOG.info("CLOSED session=%s; unfinished turn discarded", session)

    def server(self, host="127.0.0.1", port=8766):
        return serve(self.handle, host, port, process_request=self.authenticate,
                     max_size=MAX_MESSAGE, max_queue=8, write_limit=16384,
                     compression=None, open_timeout=10, close_timeout=2,
                     ping_interval=10, ping_timeout=10)


async def main(settings):
    service = VoiceService(settings)
    async with service.server():
        LOG.info("LISTEN 127.0.0.1:8766%s; DashScope; verified upstream TLS; no credentials logged", PATH)
        await asyncio.Future()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name(".private.json"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    for name in ("websockets", "httpx", "httpcore"):
        logging.getLogger(name).setLevel(logging.ERROR)
    try:
        settings = json.loads(args.config.read_text(encoding="utf-8"))
        asyncio.run(main(settings))
    except KeyboardInterrupt:
        pass
    except Exception as error:
        raise SystemExit("AI startup failed (" + type(error).__name__ + "); check private settings without printing secrets") from None
