"""DashScope adapters using bounded async HTTP/WSS, matching the original provider contracts.

No model is hosted on this ECS. API secrets never enter device messages or logs.
"""
import asyncio
import base64
import contextlib
import json
import re
import uuid
from urllib.parse import urlsplit

import httpx
from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

from audio import MAX_SAMPLES, wav_bytes

PROMPT = ("你是思巴迪，一个温和、耐心、面向小学生的纯语音伴学助手。"
          "默认用简洁普通话，每次通常不超过80个汉字、三个短句；需要时先问一个澄清问题。"
          "解释知识时给具体例子，鼓励孩子自己思考，不编造事实。"
          "只输出适合直接朗读的正文，不输出Markdown、表情或思考过程。"
          "遇到危险或明显不适龄内容时提供安全帮助，必要时建议找家长或老师。")


class ProviderFailure(RuntimeError):
    """A locally constructed diagnostic code, never an upstream response body."""
    def __init__(self, code):
        self.code = code
        super().__init__(code)


def endpoint(value, websocket=False):
    parsed = urlsplit(value)
    host = (parsed.hostname or "").lower()
    if (parsed.scheme != ("wss" if websocket else "https") or parsed.username or
            parsed.password or parsed.query or parsed.fragment or parsed.port not in (None, 443) or
            not (host in {"dashscope.aliyuncs.com", "dashscope-intl.aliyuncs.com"} or
                 (host.endswith(".maas.aliyuncs.com") and re.fullmatch(r"[a-z0-9.-]+", host)))):
        raise ValueError("Use an official DashScope HTTPS/WSS endpoint")
    expected = "/api-ws/v1/inference" if websocket else "/compatible-mode/v1"
    if parsed.path.rstrip("/") != expected:
        raise ValueError("Invalid DashScope endpoint path")
    return value.rstrip("/")


def voice_settings(settings):
    model = settings.get("tts_model", "cosyvoice-v2")
    mode = settings.get("tts_mode", "builtin")
    if not re.fullmatch(r"cosyvoice-[a-z0-9.-]+", model):
        raise ValueError("This adapter supports CosyVoice models")
    if mode == "builtin":
        voice = settings.get("tts_voice", "longxiaochun_v2")
    elif mode == "clone":
        voice = settings.get("clone_voice_id", "")
        if not voice or settings.get("clone_target_model") != model:
            raise ValueError("Clone voice requires voice ID and matching clone_target_model")
    else:
        raise ValueError("tts_mode must be builtin or clone")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,160}", voice):
        raise ValueError("Invalid voice ID")
    rate = settings.get("tts_rate", 1.0)
    if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not 0.5 <= rate <= 2.0:
        raise ValueError("tts_rate must be 0.5..2.0")
    return model, voice, rate


class DashScope:
    def __init__(self, settings):
        self.settings = settings
        self.key = settings.get("api_key", "")
        if not isinstance(self.key, str) or not self.key.strip() or any(c.isspace() for c in self.key):
            raise ValueError("Set api_key in the ignored private settings; do not print it")
        self.base = endpoint(settings["base_url"])
        self.wss = endpoint(settings["tts_ws_url"], True)
        self.model, self.voice, self.rate = voice_settings(settings)
        self.headers = {"Authorization": "Bearer " + self.key}
        self.client = None

    async def aclose(self):
        if self.client is not None:
            await self.client.aclose()
            self.client = None

    async def completion_message(self, body, timeout=30):
        # Bound the shared pool; cancelling a request still cancels its HTTP I/O.
        if self.client is None:
            self.client = httpx.AsyncClient(follow_redirects=False, trust_env=False,
                limits=httpx.Limits(max_connections=4, max_keepalive_connections=4, keepalive_expiry=30))
        response = await self.client.post(self.base + "/chat/completions", headers=self.headers,
                                         json=body, timeout=httpx.Timeout(timeout, connect=10))
        if response.status_code != 200:
            # Never log response bodies, headers or exceptions containing request data.
            raise ProviderFailure("http_" + str(response.status_code))
        message = response.json()["choices"][0]["message"]
        if not isinstance(message, dict):
            raise ValueError("Invalid completion message")
        return message

    async def completion(self, body, timeout=30):
        message = await self.completion_message(body, timeout)
        return self.content_text(message)

    @staticmethod
    def content_text(message):
        content = message.get("content")
        if isinstance(content, list):
            content = "".join(part.get("text", "") for part in content if isinstance(part, dict))
        if not isinstance(content, str):
            raise RuntimeError("DashScope returned invalid text")
        return content.strip()

    async def recognize(self, pcm):
        audio = "data:audio/wav;base64," + base64.b64encode(wav_bytes(pcm)).decode("ascii")
        return await self.completion({"model": self.settings.get("asr_model", "qwen3-asr-flash"),
            "messages": [{"role": "user", "content": [{"type": "input_audio", "input_audio": {"data": audio}}]}],
            "stream": False, "asr_options": {"enable_itn": True}})

    async def answer(self, messages):
        text = await self.completion({"model": self.settings.get("llm_model", "qwen-plus"),
            "messages": messages, "stream": False, "enable_thinking": False,
            "temperature": 0.5, "max_tokens": 256})
        return self.answer_text({"content": text})

    def answer_text(self, message):
        text = self.content_text(message)
        if not text:
            raise RuntimeError("Empty model answer")
        # Bound reply duration even if the model ignores the short-answer prompt.
        if len(text) > 180:
            text = text[:180]
            cut = max(text.rfind(mark) for mark in "。！？.!?")
            text = text[:cut + 1] if cut >= 40 else text + "。"
        return text

    async def synthesize(self, text):
        return b"".join([chunk async for chunk in self.synthesize_stream(text)])

    async def synthesize_stream(self, text):
        try:
            async with contextlib.aclosing(self._synthesize_stream(text)) as chunks:
                async for chunk in chunks:
                    yield chunk
        except InvalidStatus as error:
            raise ProviderFailure("tts_http_" + str(error.response.status_code)) from None

    async def _synthesize_stream(self, text):
        task = uuid.uuid4().hex
        def request(action, payload):
            return json.dumps({"header": {"action": action, "task_id": task, "streaming": "duplex"},
                               "payload": payload}, ensure_ascii=False)
        received_bytes = 0
        async with asyncio.timeout(40):
            async with connect(self.wss, additional_headers=self.headers, open_timeout=10,
                               close_timeout=2, max_size=262144, max_queue=8,
                               compression=None) as socket:
                await socket.send(request("run-task", {"task_group": "audio", "task": "tts",
                    "function": "SpeechSynthesizer", "model": self.model,
                    "parameters": {"text_type": "PlainText", "voice": self.voice, "format": "pcm",
                                   "sample_rate": 16000, "rate": self.rate, "volume": 50}, "input": {}}))
                started = False
                async for raw in socket:
                    if isinstance(raw, bytes):
                        if not started or received_bytes + len(raw) > MAX_SAMPLES * 2:
                            raise RuntimeError("TTS audio exceeds the bounded reply")
                        received_bytes += len(raw)
                        if raw:
                            yield raw
                        continue
                    message = json.loads(raw)
                    header = message.get("header", {})
                    if header.get("task_id") != task:
                        raise RuntimeError("Unexpected TTS task")
                    event = header.get("event")
                    if event == "task-started":
                        if started:
                            raise RuntimeError("Duplicate TTS start")
                        started = True
                        await socket.send(request("continue-task", {"input": {"text": text}}))
                        await socket.send(request("finish-task", {"input": {}}))
                    elif event == "task-failed":
                        raise ProviderFailure("tts_task_failed")
                    elif event == "task-finished":
                        if not received_bytes or received_bytes % 2:
                            raise RuntimeError("Invalid TTS PCM")
                        return
        raise RuntimeError("TTS ended without task-finished")
