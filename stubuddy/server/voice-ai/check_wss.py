"""Windows read-only WSS certificate/IP, authentication and ai/1 readiness check."""
import argparse
import asyncio
import json
from pathlib import Path
import ssl

from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

AUDIO = {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60}


async def check(settings, controls=False):
    url = f"wss://{settings['server_ip']}/stubuddy/ai"
    headers = {"Authorization": "Bearer " + settings["device_token"],
               "Device-Id": "02:00:00:00:04:02", "Protocol-Version": "1"}
    async with asyncio.timeout(20):
        async with connect(url, ssl=ssl.create_default_context(), additional_headers=headers,
                           open_timeout=10, close_timeout=2, max_size=4112, compression=None) as socket:
            await socket.send(json.dumps({"type": "hello", "transport": "websocket", "ai_protocol": 1,
                                          "reply_stream": 1, "max_reply_packet_bytes": 1275,
                                          **({"device_controls": 1} if controls else {}),
                                          **({"volume_restore": 1} if controls else {}),
                                          "audio_params": AUDIO}))
            hello = json.loads(await socket.recv())
            if (hello.get("ai_protocol") != 1 or hello.get("audio_params") != AUDIO or
                    hello.get("reply_stream") != 1 or hello.get("max_reply_packet_bytes") != 1275):
                raise ValueError("Invalid ai/1 hello")
            if controls and (hello.get("device_controls") != 1 or hello.get("volume_restore") != 1):
                raise ValueError("Missing device controls support; update server first")
            await socket.send('{"type":"ping"}')
            if json.loads(await socket.recv()).get("type") != "pong":
                raise ValueError("Missing pong")
    headers["Authorization"] = "Bearer " + "invalid-test-token"
    try:
        async with connect(url, ssl=ssl.create_default_context(), additional_headers=headers, open_timeout=10):
            raise ValueError("Invalid token accepted")
    except InvalidStatus as error:
        if error.response.status_code != 401:
            raise ValueError("Unexpected authentication status") from None
    print("PASS: WSS certificate/IP validation, ai/1 STREAM handshake (1275 bytes), heartbeat and wrong-token rejection")
    if controls:
        print("PASS: device_controls=1 volume_restore=1 negotiated; no physical volume changes performed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name(".private.json"))
    parser.add_argument("--controls", action="store_true")
    args = parser.parse_args()
    try:
        asyncio.run(check(json.loads(args.config.read_text(encoding="utf-8")), args.controls))
    except Exception as error:
        raise SystemExit("FAIL: WSS readiness (" + type(error).__name__ + "); keep TLS verification enabled") from None
