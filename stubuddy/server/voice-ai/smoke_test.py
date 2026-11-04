"""ECS-only real-model self-test; uses paid DashScope calls, never device hardware."""
import argparse
import asyncio
import json
from pathlib import Path
import zlib
import time

from websockets.asyncio.client import connect

from voice_server import AUDIO_PARAMS, Recording, pack, unpack
from providers import DashScope, ProviderFailure
from audio import encode, decode


async def check(settings):
    provider = DashScope(settings)
    pcm = await provider.synthesize("你好，请只回答你好。")
    print("PASS: DashScope TTS generated test input")
    packets, samples, delay = await asyncio.to_thread(encode, pcm)
    headers = {"Authorization": "Bearer " + settings["device_token"],
               "Device-Id": "02:00:00:00:04:01", "Protocol-Version": "1"}
    async with asyncio.timeout(100):
        async with connect("ws://127.0.0.1:8766/stubuddy/ai", additional_headers=headers,
                           max_size=4112, close_timeout=2, compression=None) as socket:
            await socket.send(json.dumps({"type": "hello", "transport": "websocket", "ai_protocol": 1,
                                          "reply_stream": 1, "max_reply_packet_bytes": 1275,
                                          "audio_params": AUDIO_PARAMS}))
            hello = json.loads(await socket.recv())
            if hello.get("ai_protocol") != 1 or hello.get("reply_stream") != 1:
                raise ValueError("No ai/1 handshake")
            await socket.send('{"type":"listen","state":"start","mode":"manual","turn":1}')
            crc = 0
            for index, packet in enumerate(packets):
                crc = zlib.crc32(packet, crc)
                await socket.send(pack(1, index, packet))
            await socket.send(json.dumps({"type": "listen", "state": "stop", "turn": 1,
                "samples": samples, "delay": delay, "frames": len(packets),
                "bytes": sum(map(len, packets)), "crc32": crc}))
            received = Recording(1)
            metadata = None
            recognized = answered = False
            checkpoints = 0
            started = time.monotonic()
            while True:
                raw = await socket.recv()
                if isinstance(raw, bytes):
                    turn, sequence, payload = unpack(raw)
                    if turn != 1 or metadata is None:
                        raise ValueError("Unexpected reply audio")
                    if len(payload) > 1275:
                        raise ValueError("Negotiated downlink packet limit exceeded")
                    received.append(sequence, payload)
                    continue
                message = json.loads(raw)
                if message.get("turn") != 1 or message.get("session_id") != hello["session_id"]:
                    raise ValueError("Reply identity mismatch")
                kind = message["type"]
                if kind == "stt":
                    recognized = bool(message.get("text"))
                    print("PASS: DashScope ASR returned text")
                elif kind == "reply_text":
                    answered = bool(message.get("text"))
                    print("PASS: DashScope LLM returned text")
                elif kind == "reply_start":
                    metadata = message
                    if message.get("stream") != 1 or message.get("max_packet_bytes") != 1275:
                        raise ValueError("Missing stream negotiation")
                elif kind == "reply_chunk":
                    if (message["frames"] != len(received.packets) or message["bytes"] != received.byte_count or
                            message["crc32"] != received.crc or message["delay"] != metadata["delay"] or
                            message["frames"] != (message["samples"] + message["delay"] + 959) // 960):
                        raise ValueError("Stream checkpoint mismatch")
                    checkpoints += 1
                    if checkpoints == 1:
                        print("PASS: first verified stream chunk before reply_end; elapsed_ms=" +
                              str(round((time.monotonic() - started) * 1000)))
                elif kind == "reply_end":
                    if metadata is None or message["delay"] != metadata["delay"] or not checkpoints:
                        raise ValueError("Reply metadata mismatch")
                    received.finish(message)
                    output = await asyncio.to_thread(decode, received.packets, message["samples"], message["delay"])
                    if not output or not recognized or not answered:
                        raise ValueError("Incomplete model chain")
                    # A PC test does not assert physical playback or commit history.
                    await socket.send('{"type":"abort","turn":1}')
                    print("PASS: real ASR/LLM/STREAM TTS, Opus decoding, checkpoint/final CRC/sample integrity; max_packet=" +
                          str(max(map(len, received.packets))))
                    print("Device microphone, speaker, LED and physical interruption still require hardware testing.")
                    return
                elif kind in {"turn_error", "turn_empty"}:
                    raise RuntimeError("Model chain failed at " + message.get("code", kind))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name(".private.json"))
    args = parser.parse_args()
    try:
        asyncio.run(check(json.loads(args.config.read_text(encoding="utf-8"))))
    except Exception as error:
        diagnostic = error.code if isinstance(error, ProviderFailure) else type(error).__name__
        raise SystemExit("FAIL: real model self-test (" + diagnostic + "); check service logs and private model settings") from None
