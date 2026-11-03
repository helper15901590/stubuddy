"""Verify the running service using the real local token, without recording audio."""
import asyncio
import argparse
import json
import zlib
from pathlib import Path

from websockets.asyncio.client import connect
from echo_protocol import AUDIO_PARAMS, pack, unpack


async def main(url_override=None):
    config = json.loads((Path(__file__).resolve().parent / ".local.json").read_text(encoding="utf-8"))
    url = url_override or f"ws://{config['host']}:{config['port']}/stubuddy/echo"
    async with asyncio.timeout(10):
        async with connect(url, additional_headers={"Authorization": "Bearer " + config["token"],
                           "Device-Id": "00:00:00:00:00:00", "Protocol-Version": "1"},
                           compression=None, open_timeout=3, close_timeout=2) as client:
            await client.send(json.dumps({"type": "hello", "transport": "websocket", "echo_protocol": 1,
                                         "audio_params": AUDIO_PARAMS}))
            hello = json.loads(await client.recv())
            if hello.get("audio_params") != AUDIO_PARAMS or hello.get("echo_protocol") != 1:
                raise ValueError("Handshake mismatch")
            await client.send('{"type":"listen","state":"start","turn":1}')
            payload = b"stubuddy-network-selftest"
            crc = 0
            for sequence in range(6):
                await client.send(pack(1, sequence, payload))
                crc = zlib.crc32(payload, crc)
            metadata = {"turn": 1, "samples": 4800, "delay": 104, "frames": 6,
                        "bytes": len(payload) * 6, "crc32": crc}
            await client.send(json.dumps({"type": "listen", "state": "stop", **metadata}))
            if json.loads(await client.recv()) != {"type": "echo_start", **metadata}:
                raise ValueError("Echo start mismatch")
            for sequence in range(6):
                if unpack(await client.recv()) != (1, sequence, payload):
                    raise ValueError("Echo audio mismatch")
            if json.loads(await client.recv()) != {"type": "echo_end", **metadata}:
                raise ValueError("Echo end mismatch")
    print("PASS: WS connection, authentication, framing and CRC match.")
    print("This tests network payloads; real Opus and hardware audio require device acceptance.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", help="Optional target override for loopback diagnostics")
    args = parser.parse_args()
    try:
        asyncio.run(main(args.url))
    except (OSError, ValueError, TimeoutError) as error:
        raise SystemExit("FAIL: start echo_server.py and check the configured LAN IP/port. " + str(error)) from error
