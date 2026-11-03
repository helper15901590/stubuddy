"""Real loopback sockets exercise the private echo service without ASR/LLM or hardware."""
import asyncio
import contextlib
import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT.parents[1] / "stubuddy/server/ws-echo"
sys.path.insert(0, str(SERVER))
import configure as echo_configure
try:
    from websockets.asyncio.client import connect
    from websockets.exceptions import InvalidStatus, ConnectionClosed
    from echo_server import EchoService
    from echo_protocol import AUDIO_PARAMS, MAX_PACKETS, ProtocolError, Recording, pack, unpack
    AVAILABLE = True
except ImportError:
    AVAILABLE = False


class EchoWireTests(unittest.TestCase):
    def test_actual_cpp_wire_crc_and_bounded_reassembly(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ("echo.exe" if os.name == "nt" else "echo")
            subprocess.run(compiler + ["-std=c++20", "-I" + str(ROOT / "main/boards/stubuddy-s3-n16r8"),
                                       str(ROOT / "scripts/tests/stubuddy_echo_test.cc"), "-o", str(executable)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


class EchoConfigurationTests(unittest.TestCase):
    def test_physical_adapter_selection_requires_an_unambiguous_address(self):
        self.assertEqual(echo_configure.select_address('"10.1.20.29"'), "10.1.20.29")
        self.assertEqual(echo_configure.select_address('["10.1.20.29","10.1.20.29"]'), "10.1.20.29")
        for value in ('[]', '["10.1.20.29","192.168.1.20"]', '{}', 'null'):
            with self.assertRaises(ValueError):
                echo_configure.select_address(value)

    def test_reconfiguration_preserves_token_and_does_not_print_it(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            settings = folder / "server"
            settings.mkdir()
            firmware = folder / "firmware"
            firmware.mkdir()
            output = io.StringIO()
            with mock.patch.object(echo_configure, "ROOT", settings), \
                    mock.patch.object(echo_configure, "FIRMWARE", firmware), contextlib.redirect_stdout(output):
                echo_configure.configure("10.1.20.29", 8765)
                first = json.loads((settings / ".local.json").read_text())
                echo_configure.configure("192.168.1.20", 8766)
                second = json.loads((settings / ".local.json").read_text())
            self.assertEqual(first["token"], second["token"])
            self.assertNotIn(first["token"], output.getvalue())
            header = (firmware / "tmp/stubuddy_echo_local.h").read_text()
            self.assertIn("ws://192.168.1.20:8766/stubuddy/echo", header)
            self.assertIn(first["token"], header)

    def test_invalid_addresses_cannot_overwrite_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            with mock.patch.object(echo_configure, "ROOT", folder), \
                    mock.patch.object(echo_configure, "FIRMWARE", folder):
                for address in ("127.0.0.1", "0.0.0.0", "8.8.8.8", "192.168.1.2\nBAD"):
                    with self.assertRaises(ValueError): echo_configure.configure(address, 8765)
                self.assertFalse((folder / ".local.json").exists())
                (folder / ".local.json").write_text("[]", encoding="utf-8")
                with self.assertRaises(ValueError): echo_configure.configure("192.168.1.2", 8765)
                self.assertEqual((folder / ".local.json").read_text(), "[]")


@unittest.skipUnless(AVAILABLE, "Install stubuddy/server/ws-echo/requirements.txt")
class EchoServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.token = "a" * 64
        self.service = EchoService(self.token, echo_delay=0.01)
        self.server = await self.service.server("127.0.0.1", 0)
        self.url = f"ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/stubuddy/echo"
        self.clients = []

    async def asyncTearDown(self):
        for client in self.clients:
            await client.close()
        self.server.close()
        await self.server.wait_closed()
        self.assertEqual(len(self.service.connections), 0)

    async def client(self, device="24:58:7c:d1:3f:c8"):
        client = await connect(self.url, additional_headers={"Authorization": "Bearer " + self.token,
                         "Device-Id": device, "Protocol-Version": "1"}, compression=None)
        self.clients.append(client)
        await client.send(json.dumps({"type": "hello", "version": 1, "transport": "websocket",
                                     "echo_protocol": 1, "audio_params": AUDIO_PARAMS}))
        hello = json.loads(await asyncio.wait_for(client.recv(), 2))
        self.assertEqual(hello["audio_params"], AUDIO_PARAMS)
        self.assertEqual(hello["echo_protocol"], 1)
        return client

    async def upload(self, client, turn=1, samples=4800, content=b"test-opus-payload"):
        frames = (samples + 104 + 959) // 960
        await client.send(json.dumps({"type": "listen", "state": "start", "turn": turn}))
        checksum = 0
        for sequence in range(frames):
            await client.send(pack(turn, sequence, content))
            checksum = zlib.crc32(content, checksum)
        metadata = {"turn": turn, "samples": samples, "delay": 104, "frames": frames,
                    "bytes": len(content) * frames, "crc32": checksum}
        await client.send(json.dumps({"type": "listen", "state": "stop", **metadata}))
        return metadata

    async def receive(self, client, expected, content):
        count = size = checksum = 0
        async with asyncio.timeout(5):
            start = json.loads(await client.recv())
            self.assertEqual(start, {"type": "echo_start", **expected})
            while True:
                raw = await client.recv()
                if isinstance(raw, str):
                    self.assertEqual(json.loads(raw), {"type": "echo_end", **expected})
                    break
                turn, sequence, payload = unpack(raw)
                self.assertEqual((turn, sequence, payload), (expected["turn"], count, content))
                checksum = zlib.crc32(payload, checksum)
                count += 1
                size += len(payload)
        self.assertEqual((count, size, checksum), (expected["frames"], expected["bytes"], expected["crc32"]))

    async def test_authenticated_round_trip_and_heartbeat(self):
        client = await self.client()
        await client.send('{"type":"ping"}')
        self.assertEqual(json.loads(await client.recv()), {"type": "pong"})
        metadata = await self.upload(client, content=b"first-and-last")
        await self.receive(client, metadata, b"first-and-last")

    async def test_full_sixty_seconds_and_repeated_rounds(self):
        client = await self.client()
        for turn, samples in ((1, 960000), (2, 4960), (3, 16000)):
            metadata = await self.upload(client, turn, samples, b"minute")
            await self.receive(client, metadata, b"minute")

    async def test_cancel_waiting_and_reject_late_previous_audio(self):
        self.service.echo_delay = 0.05
        client = await self.client()
        await self.upload(client, 1, content=b"old")
        await client.send('{"type":"abort","turn":1}')
        await client.send('{"type":"listen","state":"start","turn":2}')
        await client.send(pack(1, 0, b"late-old-data"))
        # Replace the empty new capture with a fresh turn; cancelled data must
        # not enter any later recording or resume during its echo.
        metadata = await self.upload(client, 3, content=b"new")
        await self.receive(client, metadata, b"new")

    async def test_client_audio_is_isolated(self):
        first = await self.client()
        second = await self.client("24:58:7c:d1:3f:c9")
        a = await self.upload(first, content=b"child-a")
        b = await self.upload(second, content=b"child-b")
        await asyncio.gather(self.receive(first, a, b"child-a"), self.receive(second, b, b"child-b"))

    async def test_cancel_in_flight_echo_and_drain_only_old_turn(self):
        client = await self.client()
        await self.upload(client, 1, 960000, b"old-frame" * 100)
        start = json.loads(await client.recv())
        self.assertEqual((start["type"], start["turn"]), ("echo_start", 1))
        self.assertEqual(unpack(await client.recv())[0], 1)
        await client.send('{"type":"abort","turn":1}')
        metadata = await self.upload(client, 2, content=b"replacement")
        async with asyncio.timeout(5):
            for _ in range(MAX_PACKETS + 3):
                raw = await client.recv()
                if isinstance(raw, str):
                    message = json.loads(raw)
                    if message["type"] == "echo_start" and message["turn"] == 2:
                        break
                    self.assertEqual(message["turn"], 1)
                else:
                    self.assertEqual(unpack(raw)[0], 1)
            else:
                self.fail("Replacement echo did not start")
            for sequence in range(metadata["frames"]):
                self.assertEqual(unpack(await client.recv()), (2, sequence, b"replacement"))
            self.assertEqual(json.loads(await client.recv()), {"type": "echo_end", **metadata})

    async def test_wrong_token_missing_identity_and_wrong_path_are_rejected(self):
        cases = [(self.url, {"Authorization": "Bearer " + "b" * 64, "Device-Id": "24:58:7c:d1:3f:c8", "Protocol-Version": "1"}, 401),
                 (self.url, {"Authorization": "Bearer " + self.token}, 400),
                 (self.url + "-wrong", {"Authorization": "Bearer " + self.token}, 404)]
        for url, headers, status in cases:
            with self.assertRaises(InvalidStatus) as error:
                await connect(url, additional_headers=headers)
            self.assertEqual(error.exception.response.status_code, status)

    async def test_missing_frame_and_integrity_mismatch_close_connection(self):
        client = await self.client()
        await client.send('{"type":"listen","state":"start","turn":1}')
        await client.send(pack(1, 1, b"missing-zero"))
        with self.assertRaises(ConnectionClosed):
            await client.recv()
        self.assertEqual(client.close_code, 1008)

    async def test_corrupted_crc_is_rejected_before_echo(self):
        client = await self.client()
        await client.send('{"type":"listen","state":"start","turn":1}')
        for sequence in range(6):
            await client.send(pack(1, sequence, b"x"))
        await client.send(json.dumps({"type": "listen", "state": "stop", "turn": 1,
                                     "samples": 4800, "delay": 104, "frames": 6,
                                     "bytes": 6, "crc32": 0}))
        with self.assertRaises(ConnectionClosed):
            await client.recv()
        self.assertEqual(client.close_code, 1008)

    async def test_cancel_partial_capture_then_new_round(self):
        client = await self.client()
        await client.send('{"type":"listen","state":"start","turn":1}')
        await client.send(pack(1, 0, b"short-tap"))
        await client.send('{"type":"abort","turn":1}')
        metadata = await self.upload(client, 2, content=b"fresh")
        await self.receive(client, metadata, b"fresh")

    async def test_wrong_sample_rate_is_not_accepted(self):
        client = await connect(self.url, additional_headers={"Authorization": "Bearer " + self.token,
                               "Device-Id": "24:58:7c:d1:3f:c8", "Protocol-Version": "1"})
        self.clients.append(client)
        await client.send(json.dumps({"type": "hello", "transport": "websocket", "echo_protocol": 1,
                                     "audio_params": {**AUDIO_PARAMS, "sample_rate": 24000}}))
        with self.assertRaises(ConnectionClosed):
            await client.recv()
        self.assertEqual(client.close_code, 1008)

    def test_metadata_and_wire_bounds(self):
        with self.assertRaises(ProtocolError): pack(1, MAX_PACKETS, b"x")
        with self.assertRaises(ProtocolError): pack(1, 0, b"x" * 4097)
        with self.assertRaises(ProtocolError): unpack(b"SB")
        with self.assertRaises(ProtocolError): unpack(pack(1, 0, b"x")[:-1])
        with self.assertRaises(ProtocolError): Recording(True)
        record = Recording(1)
        record.append(0, b"x")
        with self.assertRaises(ProtocolError):
            record.finish({"samples": 4800, "delay": 104, "frames": 6, "bytes": 6, "crc32": 0})
        record.clear()
        self.assertEqual(record.packets, [])


if __name__ == "__main__":
    unittest.main()
