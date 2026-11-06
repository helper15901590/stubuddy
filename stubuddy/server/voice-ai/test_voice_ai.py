"""Real local WebSocket tests; fake model calls never use credentials or paid APIs."""
import asyncio
import contextlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zlib
import array
import math

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

from history import History
from providers import endpoint, voice_settings, DashScope
from voice_server import VoiceService, AUDIO_PARAMS, pack
from install_ecs import extend_nginx
import install_ecs
import providers
import httpx
import audio
import configure
import io
import tarfile

try:
    import opuslib_next
    NATIVE_OPUS = True
except Exception:
    NATIVE_OPUS = False


def sine_pcm(samples):
    data = array.array("h", (int(6000 * math.sin(index * 2 * math.pi * 330 / 16000)) for index in range(samples)))
    return data.tobytes()


@unittest.skipUnless(NATIVE_OPUS, "Install native libopus (Ubuntu: libopus0) for real codec tests")
class AudioTests(unittest.TestCase):
    def test_continuous_stream_irregular_chunks_exact_tail_and_packet_ceiling(self):
        for samples in (1, 4800, 16003, 960000):
            source = sine_pcm(samples)
            encoder = audio.StreamEncoder()
            packets = []
            for offset in range(0, len(source), 3333):
                packets.extend(encoder.feed(source[offset:offset + 3333]))
            packets.extend(encoder.finish())
            expected = max(samples, 4800)
            self.assertEqual(encoder.samples, expected)
            self.assertEqual(len(packets), (expected + encoder.delay + 959) // 960)
            self.assertLessEqual(max(map(len, packets)), 1275)
            output = audio.decode(packets, expected, encoder.delay)
            self.assertEqual(len(output), expected * 2)
            if samples > 4800:
                returned = array.array('h', output)[1000:samples - 1000]
                original = array.array('h', source)[1000:-1000]
                correlation = sum(a*b for a,b in zip(original, returned)) / math.sqrt(
                    sum(a*a for a in original) * sum(b*b for b in returned))
                self.assertGreater(correlation, .9)
                self.assertGreater(sum(abs(v) for v in array.array('h', output)[-960:]), 100000)

    def test_stream_rejects_odd_empty_and_sixty_second_overflow(self):
        for source in (b'', b'x'):
            encoder = audio.StreamEncoder()
            encoder.feed(source)
            with self.assertRaises(ValueError): encoder.finish()
        with self.assertRaises(ValueError): audio.StreamEncoder().feed(bytes(1920002))

    def test_real_opus_independent_lengths_delay_alignment_and_sixty_second_tail(self):
        for samples in (4800, 16003, 960000):
            source = sine_pcm(samples)
            packets, actual, delay = audio.encode(source)
            self.assertEqual(actual, samples)
            self.assertEqual(len(packets), (samples + delay + 959) // 960)
            decoded = audio.decode(packets, samples, delay)
            self.assertEqual(len(decoded), len(source))
            original = array.array("h", source)
            returned = array.array("h", decoded)
            x = original[1000:-1000]
            y = returned[1000:-1000]
            correlation = sum(a * b for a, b in zip(x, y)) / math.sqrt(sum(a * a for a in x) * sum(b * b for b in y))
            self.assertGreater(correlation, 0.9)
            self.assertGreater(sum(abs(value) for value in returned[:960]), 100000)
            self.assertGreater(sum(abs(value) for value in returned[-960:]), 100000)

    def test_empty_oversized_and_odd_pcm_are_rejected(self):
        for value in (b"", b"x", bytes(1920002)):
            with self.assertRaises(ValueError):
                audio.encode(value)


class WindowTests(unittest.TestCase):
    def test_eight_historical_pairs_plus_current_and_slide_after_completion(self):
        history = History()
        for turn in range(1, 9):
            history.commit("Q" + str(turn), "A" + str(turn))
        messages = history.messages("system", "Q9")
        self.assertEqual([m["content"] for m in messages],
                         ["system"] + [item for i in range(1, 9) for item in (f"Q{i}", f"A{i}")] + ["Q9"])
        # Preparing/cancelling Q9 must not evict the previously completed Q1.
        self.assertEqual(history.pairs[0], ("Q1", "A1"))
        history.commit("Q9", "A9")
        self.assertEqual(list(history.pairs), [(f"Q{i}", f"A{i}") for i in range(2, 10)])
        self.assertEqual(history.messages("system", "Q10")[1]["content"], "Q2")
        history.commit("Q10", "A10")
        self.assertEqual(list(history.pairs), [(f"Q{i}", f"A{i}") for i in range(3, 11)])

    def test_one_slot_and_idle_expiry(self):
        history = History(rounds=1, idle_seconds=10)
        history.commit("old", "answer")
        self.assertEqual(len(history.messages("system", "current")), 4)
        history.touched -= 11
        self.assertEqual(len(history.messages("system", "new")), 2)
        self.assertEqual(len(history.pairs), 0)

    def test_budget_rejects_without_silently_reducing_window(self):
        history = History(budget=2048)
        history.commit("a" * 1500, "b" * 500)
        with self.assertRaises(ValueError):
            history.messages("system", "x" * 100)
        self.assertEqual(len(history.pairs), 1)


class ConfigurationTests(unittest.TestCase):
    def deployment_fixture(self, directory):
        root = Path(directory) / "upload"
        target = Path(directory) / "installed"
        root.mkdir()
        target.mkdir()
        (target / ".stubuddy-ai").touch()
        staged = {"server_ip": "121.41.86.40", "device_token": "0" * 64,
                  "api_key": "staged-fixture", "tts_rate": 1.0}
        server = {**staged, "api_key": "server-fixture", "tts_rate": 0.9}
        for name in install_ecs.FILES:
            (root / name).write_text("# new code fixture\n")
        (root / ".private.json").write_text(json.dumps(staged))
        (target / ".private.json").write_text(json.dumps(server))
        return root, target, staged, server

    def test_redeployment_preserves_server_settings_but_updates_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root, target, staged, server = self.deployment_fixture(directory)
            with mock.patch.object(install_ecs, "ROOT", root), mock.patch.object(install_ecs, "TARGET", target):
                self.assertEqual(install_ecs.selected_settings(), server)
                install_ecs.copy_payload()
            self.assertEqual(json.loads((target / ".private.json").read_text()), server)
            self.assertEqual((target / "voice_server.py").read_text(), "# new code fixture\n")

    def test_explicit_config_update_and_first_install_use_staged_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root, target, staged, server = self.deployment_fixture(directory)
            with mock.patch.object(install_ecs, "ROOT", root), mock.patch.object(install_ecs, "TARGET", target):
                self.assertEqual(install_ecs.selected_settings(True), staged)
                install_ecs.copy_payload(True)
                self.assertEqual(json.loads((target / ".private.json").read_text()), staged)
                self.assertEqual(json.loads((target / ".private.before-update.json").read_text()), server)
                (target / ".private.json").unlink()
                self.assertEqual(install_ecs.selected_settings(), staged)
                install_ecs.copy_payload()
                self.assertEqual(json.loads((target / ".private.json").read_text()), staged)

    def test_retained_config_rejects_device_profile_mismatch_without_replacing_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root, target, staged, server = self.deployment_fixture(directory)
            (root / ".private.json").write_text(json.dumps({**staged, "device_token": "1" * 64}))
            with mock.patch.object(install_ecs, "ROOT", root), mock.patch.object(install_ecs, "TARGET", target):
                with self.assertRaises(ValueError):
                    install_ecs.selected_settings()
            self.assertEqual(json.loads((target / ".private.json").read_text()), server)

    def test_existing_unmanaged_target_is_rejected_before_config_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root, target, staged, server = self.deployment_fixture(directory)
            (target / ".stubuddy-ai").unlink()
            with mock.patch.object(install_ecs, "ROOT", root), mock.patch.object(install_ecs, "TARGET", target):
                with self.assertRaises(ValueError):
                    install_ecs.selected_settings()

    def test_private_package_allowlist_key_never_enters_firmware_and_token_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "voice-ai"
            root.mkdir()
            echo = root.parent / "ws-echo"
            echo.mkdir()
            (echo / "echo_protocol.py").write_text("# protocol fixture\n")
            (echo / ".cloud.json").write_text("existing echo fixture")
            for name in configure.FILES:
                (root / name).write_text("# fixture\n")
            firmware = Path(directory) / "firmware"
            settings = {"device_token": "0" * 64, "api_key": "fixture-not-a-real-key"}
            (root / ".private.json").write_text(json.dumps(settings))
            with mock.patch.object(configure, "ROOT", root), mock.patch.object(configure, "FIRMWARE", firmware), contextlib.redirect_stdout(io.StringIO()) as output:
                configure.configure("121.41.86.40")
                configure.configure("121.41.86.40")
            header = (firmware / "tmp/stubuddy_ai_private.h").read_text()
            self.assertNotIn(settings["api_key"], header)
            self.assertNotIn(settings["api_key"], output.getvalue())
            self.assertEqual(json.loads((root / ".private.json").read_text())["device_token"], "0" * 64)
            self.assertEqual((echo / ".cloud.json").read_text(), "existing echo fixture")
            with tarfile.open(root / ".deploy.tar.gz") as archive:
                self.assertEqual(set(archive.getnames()), set(configure.FILES) | {"echo_protocol.py", ".private.json"})

    def test_builtin_is_default_and_clone_requires_matching_model(self):
        self.assertEqual(voice_settings({}), ("cosyvoice-v2", "longxiaochun_v2", 1.0))
        with self.assertRaises(ValueError):
            voice_settings({"tts_mode": "clone", "clone_voice_id": "voice-demo", "clone_target_model": "cosyvoice-v1"})
        self.assertEqual(voice_settings({"tts_mode": "clone", "clone_voice_id": "voice-demo",
                                         "clone_target_model": "cosyvoice-v2"})[1], "voice-demo")

    def test_endpoints_require_verified_official_transport(self):
        self.assertIn("dashscope", endpoint("https://dashscope.aliyuncs.com/compatible-mode/v1"))
        self.assertIn("maas", endpoint("wss://test.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference", True))
        for url in ("http://dashscope.aliyuncs.com/compatible-mode/v1",
                    "https://dashscope.aliyuncs.com.evil.test/compatible-mode/v1",
                    "https://key@dashscope.aliyuncs.com/compatible-mode/v1"):
            with self.assertRaises(ValueError):
                endpoint(url)

    def test_nginx_extension_preserves_echo_and_is_idempotent(self):
        old = "server {\n    location = /stubuddy/echo {\n        proxy_pass http://127.0.0.1:8765;\n    }\n}\n"
        new = extend_nginx(old)
        self.assertIn(old[old.index("    location = /stubuddy/echo"):], new)
        self.assertIn("127.0.0.1:8766", new)
        self.assertEqual(extend_nginx(new), new)
        with self.assertRaises(ValueError):
            extend_nginx("server { location /another { } }")


class FakeProvider:
    def __init__(self):
        self.calls = 0
        self.queries = []
        self.delay = None
        self.empty = False
        self.fail = False
        self.started = asyncio.Event()
        self.tts_gate = None
        self.tts_closed = False
        self.tts_fail = False

    async def recognize(self, pcm):
        self.calls += 1
        number = self.calls
        self.started.set()
        if self.delay and number == 1:
            await self.delay.wait()
        if self.fail:
            raise RuntimeError("private-test-value-must-not-be-logged")
        return "" if self.empty else f"Q{number}"

    async def answer(self, messages):
        self.queries.append(messages)
        return "A" + messages[-1]["content"][1:]

    async def synthesize(self, text):
        return bytes(9600)

    async def synthesize_stream(self, text):
        try:
            yield sine_pcm(4800)
            if self.tts_gate:
                await self.tts_gate.wait()
            if self.tts_fail:
                raise RuntimeError("private-tts-error-must-not-be-logged")
            yield sine_pcm(4803)
        finally:
            self.tts_closed = True


class ProviderContractTests(unittest.IsolatedAsyncioTestCase):
    def settings(self, **extra):
        return {"api_key": "fixture-not-a-real-key",
                "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
                "tts_ws_url": "wss://dashscope.aliyuncs.com/api-ws/v1/inference", **extra}

    async def test_real_http_adapter_builds_asr_and_llm_contract_without_external_calls(self):
        requests = []
        def handle(request):
            requests.append(json.loads(request.content))
            self.assertEqual(request.url.path, "/compatible-mode/v1/chat/completions")
            return httpx.Response(200, json={"choices": [{"message": {"content": "你好"}}]})
        client_type = httpx.AsyncClient
        with mock.patch.object(providers.httpx, "AsyncClient", side_effect=lambda **kw:
                               client_type(transport=httpx.MockTransport(handle), **kw)):
            provider = DashScope(self.settings())
            self.assertEqual(await provider.recognize(bytes(9600)), "你好")
            pooled_client = provider.client
            await provider.answer([{"role": "user", "content": "你好"}])
            self.assertIs(provider.client, pooled_client)
            await provider.aclose()
            self.assertTrue(pooled_client.is_closed)
        self.assertEqual(requests[0]["model"], "qwen3-asr-flash")
        self.assertTrue(requests[0]["messages"][0]["content"][0]["input_audio"]["data"].startswith("data:audio/wav;base64,"))
        self.assertEqual(requests[0]["asr_options"], {"enable_itn": True})
        self.assertFalse(requests[1]["enable_thinking"])
        self.assertEqual(requests[1]["max_tokens"], 256)

    async def test_cancelled_http_request_does_not_poison_shared_pool(self):
        entered = asyncio.Event()
        calls = 0
        async def handle(request):
            nonlocal calls
            calls += 1
            if calls == 1:
                entered.set()
                await asyncio.Future()
            return httpx.Response(200, json={"choices":[{"message":{"content":"3"}}]})
        provider = DashScope(self.settings())
        provider.client = httpx.AsyncClient(transport=httpx.MockTransport(handle))
        try:
            task = asyncio.create_task(provider.answer([{"role":"user","content":"old"}]))
            await asyncio.wait_for(entered.wait(), 2)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertEqual(await provider.answer([{"role":"user","content":"new"}]), "3")
        finally:
            await provider.aclose()

    async def test_tts_run_continue_finish_and_clone_voice_share_one_contract(self):
        class Socket:
            def __init__(self):
                self.requests = []
                self.index = 0
                self.closed = False
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                self.closed = True
            async def send(self, raw):
                self.requests.append(json.loads(raw))
            def __aiter__(self):
                return self
            async def __anext__(self):
                self.index += 1
                task = self.requests[0]["header"]["task_id"]
                if self.index == 2:
                    return bytes(9600)
                if self.index > 3:
                    raise StopAsyncIteration
                event = "task-started" if self.index == 1 else "task-finished"
                return json.dumps({"header": {"task_id": task, "event": event}})
        for settings, expected_voice in (({}, "longxiaochun_v2"),
            ({"tts_mode": "clone", "clone_voice_id": "clone-fixture", "clone_target_model": "cosyvoice-v2"}, "clone-fixture")):
            socket = Socket()
            with mock.patch.object(providers, "connect", return_value=socket):
                pcm = await DashScope(self.settings(**settings)).synthesize("你好")
            self.assertEqual(len(pcm), 9600)
            self.assertTrue(socket.closed)
            self.assertEqual([m["header"]["action"] for m in socket.requests], ["run-task", "continue-task", "finish-task"])
            self.assertEqual(len({m["header"]["task_id"] for m in socket.requests}), 1)
            self.assertEqual(socket.requests[0]["payload"]["parameters"]["voice"], expected_voice)
            self.assertEqual(socket.requests[0]["payload"]["parameters"]["sample_rate"], 16000)
        socket = Socket()
        with mock.patch.object(providers, "connect", return_value=socket):
            stream = DashScope(self.settings()).synthesize_stream("你好")
            self.assertEqual(len(await anext(stream)), 9600)
            self.assertFalse(socket.closed)  # Audio is available before task-finished.
            await stream.aclose()
        self.assertTrue(socket.closed)  # Physical cancellation closes upstream WSS now.


def fake_encode(pcm):
    return [b"fake-opus"] * 5, 4800, 0


class SessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.stack = contextlib.AsyncExitStack()
        self.provider = FakeProvider()
        self.service = VoiceService({"device_token": "0" * 64}, self.provider,
                                    (lambda *args: bytes(9600), fake_encode))
        server = await self.stack.enter_async_context(self.service.server("127.0.0.1", 0))
        self.url = "ws://127.0.0.1:" + str(server.sockets[0].getsockname()[1]) + "/stubuddy/ai"

    async def asyncTearDown(self):
        await self.stack.aclose()

    async def client(self, device="aa:bb:cc:dd:ee:01", token=None, protocol="ai_protocol", streaming=False):
        headers = {"Device-Id": device, "Authorization": "Bearer " + (token or "0" * 64), "Protocol-Version": "1"}
        socket = await self.stack.enter_async_context(connect(self.url, additional_headers=headers))
        await socket.send(json.dumps({"type": "hello", "transport": "websocket", protocol: 1, "audio_params": AUDIO_PARAMS,
            **({"reply_stream": 1, "max_reply_packet_bytes": 1275} if streaming else {})}))
        hello = json.loads(await socket.recv())
        self.assertEqual(hello["ai_protocol"], 1)
        if streaming:
            self.assertEqual(hello["reply_stream"], 1)
            self.assertEqual(hello["max_reply_packet_bytes"], 1275)
        return socket

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_stream_keeps_generated_text_before_tts_finishes_and_ack_does_not_duplicate(self):
        self.provider.tts_gate = asyncio.Event()
        socket = await self.client(streaming=True)
        await self.upload(socket, 1)
        messages = await self.until(socket, "reply_chunk")
        self.assertFalse(any(m["type"] == "reply_end" for m in messages))
        self.assertEqual(messages[-1]["samples"], 4800 - 104)
        self.assertFalse(self.provider.tts_closed)
        # Generated text is already stored; playback acknowledgements never duplicate it.
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), [("Q1", "A1")])
        self.provider.tts_gate.set()
        await self.until(socket, "reply_end")
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), [("Q1", "A1")])

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_stream_abort_closes_upstream_but_next_turn_remembers_full_generated_answer(self):
        self.provider.tts_gate = asyncio.Event()
        socket = await self.client(streaming=True)
        await self.upload(socket, 1)
        await self.until(socket, "reply_chunk")
        await socket.send('{"type":"abort","turn":1}')
        await self.drain(socket)
        self.assertTrue(self.provider.tts_closed)
        self.provider.tts_gate = None
        await self.upload(socket, 2)
        messages = await self.until(socket, "reply_end")
        self.assertTrue(all(m.get("turn") == 2 for m in messages))
        await socket.send('{"type":"playback_done","turn":1}')
        await socket.send('{"type":"playback_done","turn":2}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), [("Q1", "A1"), ("Q2", "A2")])
        self.assertEqual([m['content'] for m in self.provider.queries[-1]][1:], ['Q1', 'A1', 'Q2'])

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_stream_final_crc_exact_length_and_monotonic_checkpoints(self):
        socket = await self.client(streaming=True)
        await self.upload(socket, 1)
        packets = []
        checkpoints = []
        from voice_server import unpack
        async with asyncio.timeout(3):
            while True:
                raw = await socket.recv()
                if isinstance(raw, bytes):
                    turn, seq, payload = unpack(raw)
                    self.assertEqual((turn, seq), (1, len(packets)))
                    self.assertLessEqual(len(payload), 1275)
                    packets.append(payload)
                else:
                    m = json.loads(raw)
                    if m['type'] not in ('reply_chunk', 'reply_end'): continue
                    self.assertEqual(m['frames'], len(packets))
                    crc = 0
                    for packet in packets: crc = zlib.crc32(packet, crc)
                    self.assertEqual(m['crc32'], crc)
                    self.assertEqual(m['bytes'], sum(map(len, packets)))
                    previous = checkpoints[-1]['frames'] if checkpoints else 0
                    self.assertLessEqual(m['frames'] - previous, 5)
                    checkpoints.append(m)
                    if m['type'] == 'reply_end':
                        self.assertEqual(m['samples'], 9603)
                        self.assertEqual(len(audio.decode(packets, m['samples'], m['delay'])), 19206)
                        break

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_tts_failure_retains_generated_answer_and_next_turn_recovers(self):
        self.provider.tts_gate = asyncio.Event()
        socket = await self.client(streaming=True)
        await self.upload(socket, 1)
        await self.until(socket, 'reply_chunk')
        self.provider.tts_fail = True
        with self.assertLogs('stubuddy.ai', level='WARNING') as logs:
            self.provider.tts_gate.set()
            messages = await self.until(socket, 'turn_error')
        self.assertEqual(messages[-1]['code'], 'tts')
        self.assertNotIn('private-tts-error', ' '.join(logs.output))
        self.assertTrue(self.provider.tts_closed)
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories['aa:bb:cc:dd:ee:01'].pairs), [('Q1', 'A1')])
        self.provider.tts_fail = False
        self.provider.tts_gate = None
        await self.upload(socket, 2)
        await self.until(socket, 'reply_end')
        await socket.send('{"type":"playback_done","turn":2}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories['aa:bb:cc:dd:ee:01'].pairs), [('Q1', 'A1'), ('Q2', 'A2')])

    async def start(self, socket, turn):
        await socket.send(json.dumps({"type": "listen", "state": "start", "mode": "manual", "turn": turn}))

    async def upload(self, socket, turn, crc_error=False):
        await self.start(socket, turn)
        payload = b"uploaded-opus"
        crc = 0
        for index in range(5):
            crc = zlib.crc32(payload, crc)
            await socket.send(pack(turn, index, payload))
        await socket.send(json.dumps({"type": "listen", "state": "stop", "turn": turn,
                                     "samples": 4800, "delay": 0, "frames": 5,
                                     "bytes": 5 * len(payload), "crc32": crc ^ int(crc_error)}))

    async def until(self, socket, kind):
        messages = []
        async with asyncio.timeout(3):
            while True:
                raw = await socket.recv()
                if isinstance(raw, str):
                    message = json.loads(raw)
                    messages.append(message)
                    if message["type"] == kind:
                        return messages

    async def drain(self, socket):
        await socket.send('{"type":"ping"}')
        await self.until(socket, "pong")

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_generated_answers_slide_at_9_10_even_when_all_playbacks_aborted(self):
        socket = await self.client(streaming=True)
        for turn in range(1, 11):
            await self.upload(socket, turn)
            await self.until(socket, "reply_end")
            history = self.service.histories["aa:bb:cc:dd:ee:01"]
            self.assertEqual(len(history.pairs), min(turn, 8))
            await socket.send(json.dumps({"type": "playback_done", "turn": turn}))
            await socket.send(json.dumps({"type": "playback_done", "turn": turn}))
            await socket.send(json.dumps({"type": "abort", "turn": turn}))
            await self.drain(socket)
            self.assertEqual(len(history.pairs), min(turn, 8))
        self.assertEqual(self.provider.queries[8][1]["content"], "Q1")
        self.assertEqual(self.provider.queries[9][1]["content"], "Q2")
        self.assertEqual(len(self.provider.queries[8]), 18)
        self.assertEqual(list(history.pairs), [(f"Q{i}", f"A{i}") for i in range(3, 11)])

    async def test_wait_abort_continues_new_turn_and_never_commits_old(self):
        self.provider.delay = asyncio.Event()
        socket = await self.client()
        await self.upload(socket, 1)
        await self.until(socket, "stage")
        await asyncio.wait_for(self.provider.started.wait(), 2)
        await socket.send('{"type":"abort","turn":1}')
        await self.upload(socket, 2)
        messages = await self.until(socket, "reply_end")
        self.assertTrue(all(m.get("turn") == 2 for m in messages))
        await socket.send('{"type":"playback_done","turn":1}')
        await socket.send('{"type":"playback_done","turn":2}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), [("Q2", "A2")])

    async def test_playback_abort_ignores_late_ack_and_preserves_completed_history(self):
        socket = await self.client()
        await self.upload(socket, 1)
        await self.until(socket, "reply_end")
        await socket.send('{"type":"abort","turn":1}')
        await self.start(socket, 2)
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), [("Q1", "A1")])

    async def test_abort_before_first_tts_audio_still_preserves_full_answer(self):
        async def stalled_tts(text):
            await asyncio.Future()
            yield b''
        self.provider.synthesize_stream = stalled_tts
        socket = await self.client(streaming=True)
        await self.upload(socket, 1)
        await self.until(socket, 'reply_text')
        await socket.send('{"type":"abort","turn":1}')
        await self.drain(socket)
        self.provider.synthesize_stream = FakeProvider.synthesize_stream.__get__(self.provider)
        await self.upload(socket, 2)
        await self.until(socket, 'reply_end')
        self.assertEqual([m['content'] for m in self.provider.queries[-1]][1:], ['Q1', 'A1', 'Q2'])

    async def test_late_llm_return_after_cancel_cannot_store_an_unfinished_old_turn(self):
        started = asyncio.Event()
        original_answer = self.provider.answer
        async def answer(messages):
            if messages[-1]['content'] == 'Q1':
                started.set()
                try:
                    await asyncio.Future()
                except asyncio.CancelledError:
                    return 'late old result'
            return await original_answer(messages)
        self.provider.answer = answer
        socket = await self.client()
        await self.upload(socket, 1)
        await asyncio.wait_for(started.wait(), 2)
        # New listen.start also cancels, without requiring a separate abort.
        await self.upload(socket, 2)
        await self.until(socket, 'reply_end')
        self.assertEqual(list(self.service.histories['aa:bb:cc:dd:ee:01'].pairs), [('Q2','A2')])
        self.assertEqual(len(self.provider.queries[-1]), 2)

    async def test_empty_and_provider_failure_end_without_memory_commit(self):
        socket = await self.client()
        self.provider.empty = True
        await self.upload(socket, 1)
        await self.until(socket, "turn_empty")
        self.provider.empty = False
        self.provider.fail = True
        with self.assertLogs("stubuddy.ai", level="WARNING") as logs:
            await self.upload(socket, 2)
            await self.until(socket, "turn_error")
        self.assertNotIn("private-test-value", " ".join(logs.output))
        self.assertEqual(len(self.service.histories["aa:bb:cc:dd:ee:01"].pairs), 0)

    async def test_wrong_token_and_echo_protocol_are_rejected(self):
        with self.assertRaises(InvalidStatus):
            await self.client(token="1" * 64)
        with self.assertRaises(ConnectionClosed):
            await self.client(protocol="echo_protocol")

    async def test_bad_crc_never_calls_asr(self):
        socket = await self.client()
        await self.upload(socket, 1, True)
        with self.assertRaises(ConnectionClosed):
            await socket.recv()
        self.assertEqual(self.provider.calls, 0)

    async def test_histories_are_isolated_by_device(self):
        first = await self.client()
        await self.upload(first, 1)
        await self.until(first, "reply_end")
        await first.send('{"type":"playback_done","turn":1}')
        await self.drain(first)
        second = await self.client("aa:bb:cc:dd:ee:02")
        await self.upload(second, 1)
        await self.until(second, "reply_end")
        self.assertEqual(len(self.provider.queries[-1]), 2)

    @unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
    async def test_real_opus_sixty_second_uplink_and_short_independent_tts_reply(self):
        self.service.decode, self.service.encode = audio.decode, audio.encode
        socket = await self.client()
        packets, samples, delay = await asyncio.to_thread(audio.encode, sine_pcm(960000))
        await self.start(socket, 1)
        crc = 0
        for index, packet in enumerate(packets):
            crc = zlib.crc32(packet, crc)
            await socket.send(pack(1, index, packet))
        await socket.send(json.dumps({"type": "listen", "state": "stop", "turn": 1,
            "samples": samples, "delay": delay, "frames": len(packets), "bytes": sum(map(len, packets)), "crc32": crc}))
        received = []
        async with asyncio.timeout(5):
            while True:
                raw = await socket.recv()
                if isinstance(raw, bytes):
                    from voice_server import unpack
                    received.append(unpack(raw)[2])
                else:
                    message = json.loads(raw)
                    if message["type"] == "reply_end":
                        self.assertEqual(message["samples"], 4800)
                        pcm = audio.decode(received, message["samples"], message["delay"])
                        self.assertEqual(len(pcm), 9600)
                        break

    async def test_reconnect_retains_generated_answers_including_unplayed_pair(self):
        socket = await self.client()
        await self.upload(socket, 1)
        await self.until(socket, "reply_end")
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        await self.upload(socket, 2)
        await self.until(socket, "reply_end")
        await socket.close()
        for _ in range(20):
            if "aa:bb:cc:dd:ee:01" not in self.service.connected:
                break
            await asyncio.sleep(0.01)
        again = await self.client()
        await self.upload(again, 1)
        await self.until(again, "reply_end")
        prompt, *conversation = [m['content'] for m in self.provider.queries[-1]]
        self.assertTrue(prompt.startswith(self.service.prompt))
        self.assertIn('本次连接实际开放的功能', prompt)
        self.assertEqual(conversation, ['Q1', 'A1', 'Q2', 'A2', 'Q3'])


if __name__ == "__main__":
    unittest.main()
