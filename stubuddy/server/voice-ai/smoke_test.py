"""ECS-only real-model self-test; uses paid DashScope calls, never device hardware."""
import argparse
import asyncio
import json
from pathlib import Path
import zlib
import time
import re

from websockets.asyncio.client import connect

from voice_server import AUDIO_PARAMS, Recording, pack, unpack
from providers import DashScope, ProviderFailure, PROMPT
from audio import encode, decode, wav_bytes
from device_tools import GET, SET, RESTORE, answer_with_tools


async def check(settings, controls=False, math_check=False, restore=False, compare_dir=None):
    provider = DashScope(settings)
    try:
        if compare_dir is not None:
            compare_dir.mkdir(parents=True, exist_ok=True)
            for number, spoken in [(60, '六十'), (50, '五十')]:
                for label, value in [('digits', str(number)), ('chinese', spoken)]:
                    text = '已将对话音量调整到百分之' + value + '。'
                    pcm = await provider.synthesize(text)
                    (compare_dir / f'{number}-{label}.wav').write_bytes(wav_bytes(pcm))
                    print(f'PASS: raw TTS WAV {number}-{label}.wav; no Opus or device gain')
        else:
            await check_provider(settings, provider, controls, math_check, restore)
    finally:
        await provider.aclose()


async def check_provider(settings, provider, controls, math_check, restore):
    if math_check:
        async def forbidden_tool(*args):
            raise ValueError("Arithmetic must not invoke device tools")
        messages = [{"role":"system","content":PROMPT},
                    {"role":"user","content":"把音量设为90"},
                    {"role":"assistant","content":"已将对话音量调整到百分之90。"},
                    {"role":"user","content":"1+1等于多少？只说结果。"}]
        first = await answer_with_tools(provider,messages,forbidden_tool,True)
        second = await answer_with_tools(provider,messages + [{"role":"assistant","content":first},
            {"role":"user","content":"再加1，只说结果。"}],forbidden_tool,True)
        def expected_result(text, number):
            word = "二两" if number == 2 else "三"
            if re.search(r"(?<![零一二两三四五六七八九十百千万])[" + word + r"][。.!！\s]*$", text):
                return True
            values = re.findall(r"[-+]?\d+(?:\.\d+)?", text)
            if values:
                return float(values[-1]) == number
            return False
        if not expected_result(first,2) or not expected_result(second,3) or "音量" in first + second:
            raise ValueError("Arithmetic context regression")
        print("PASS: real LLM math context 1+1 -> 2, plus 1 -> 3; device tools unavailable")
    input_text = "恢复音量。" if restore else "请把设备音量调到百分之三十。" if controls else "你好，请只回答你好。"
    pcm = await provider.synthesize(input_text)
    print("PASS: DashScope TTS generated test input")
    packets, samples, delay = await asyncio.to_thread(encode, pcm)
    headers = {"Authorization": "Bearer " + settings["device_token"],
               "Device-Id": "02:00:00:00:04:01", "Protocol-Version": "1"}
    async with asyncio.timeout(100):
        async with connect("ws://127.0.0.1:8766/stubuddy/ai", additional_headers=headers,
                           max_size=4112, close_timeout=2, compression=None) as socket:
            await socket.send(json.dumps({"type": "hello", "transport": "websocket", "ai_protocol": 1,
                                          "reply_stream": 1, "max_reply_packet_bytes": 1275,
                                          **({"device_controls": 1} if controls else {}),
                                          **({"volume_restore": 1} if controls else {}),
                                          "audio_params": AUDIO_PARAMS}))
            hello = json.loads(await socket.recv())
            if hello.get("ai_protocol") != 1 or hello.get("reply_stream") != 1:
                raise ValueError("No ai/1 handshake")
            if controls and (hello.get("device_controls") != 1 or hello.get("volume_restore") != 1):
                raise ValueError("Missing device controls negotiation")
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
            simulated_volume = 0 if restore else 50
            tool_calls = []
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
                if kind == "device_tool" and controls:
                    name = message.get("name")
                    if name not in (GET, SET, RESTORE):
                        raise ValueError("Unexpected device tool")
                    if name == SET:
                        if GET not in tool_calls or message.get("volume") != 30:
                            raise ValueError("Unexpected volume plan")
                        simulated_volume = 30
                    if name == RESTORE:
                        if not restore or GET not in tool_calls:
                            raise ValueError("Unexpected restore plan")
                        simulated_volume = 30  # Simulated device's saved nonzero volume.
                    tool_calls.append(name)
                    await socket.send(json.dumps({"type": "tool_result", "turn": 1,
                        "session_id": hello["session_id"], "call_id": message["call_id"],
                        "ok": True, "volume": simulated_volume}))
                elif kind == "stt":
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
                    if controls:
                        if tool_calls != [GET, RESTORE if restore else SET] or simulated_volume != 30:
                            raise ValueError("Missing confirmed volume tools")
                        print("PASS: real DashScope Function Calling + simulated device query/set receipts (not physical NVS/audio)")
                    # Generated text is stored for this dedicated test device;
                    # a PC test never asserts physical playback completion.
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
    parser.add_argument("--controls", action="store_true")
    parser.add_argument("--math", action="store_true")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--tts-compare-dir", type=Path, help="Export four raw TTS WAVs for listening; paid calls")
    args = parser.parse_args()
    if args.tts_compare_dir and (args.controls or args.math or args.restore):
        parser.error("Run --tts-compare-dir separately from the WSS/provider smoke test")
    if args.restore and not args.controls:
        parser.error("--restore requires --controls")
    try:
        asyncio.run(check(json.loads(args.config.read_text(encoding="utf-8")), args.controls, args.math, args.restore, args.tts_compare_dir))
    except Exception as error:
        diagnostic = error.code if isinstance(error, ProviderFailure) else type(error).__name__
        raise SystemExit("FAIL: real model self-test (" + diagnostic + "); check service logs and private model settings") from None
