"""stubuddy echo/1: a negotiated test extension, not raw XiaoZhi v1 binary audio."""
import json
import struct
import zlib

HEADER = struct.Struct("!2sBBIII")
MAX_PAYLOAD = 4096
MAX_PACKETS = 1001
MAX_SAMPLES = 960000
MIN_SAMPLES = 4800
FRAME_SAMPLES = 960
MAX_MESSAGE = HEADER.size + MAX_PAYLOAD
AUDIO_PARAMS = {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60}
PATH = "/stubuddy/echo"


class ProtocolError(ValueError):
    pass


def number(value, name, maximum=0xFFFFFFFF, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError("invalid " + name)
    return value


def control(raw):
    if not isinstance(raw, str) or len(raw.encode("utf-8")) > 1024:
        raise ProtocolError("invalid control size")
    try:
        value = json.loads(raw)
    except (ValueError, RecursionError) as error:
        raise ProtocolError("invalid JSON") from error
    if type(value) is not dict or type(value.get("type")) is not str:
        raise ProtocolError("invalid control type")
    return value


def pack(turn, sequence, payload):
    number(turn, "turn", minimum=1)
    number(sequence, "sequence", maximum=MAX_PACKETS - 1)
    if not isinstance(payload, bytes) or not 0 < len(payload) <= MAX_PAYLOAD:
        raise ProtocolError("invalid Opus size")
    return HEADER.pack(b"SB", 1, 0, turn, sequence, len(payload)) + payload


def unpack(raw):
    if not isinstance(raw, bytes) or not HEADER.size < len(raw) <= MAX_MESSAGE:
        raise ProtocolError("invalid audio size")
    magic, version, kind, turn, sequence, length = HEADER.unpack_from(raw)
    if magic != b"SB" or version != 1 or kind != 0:
        raise ProtocolError("invalid audio header")
    number(turn, "turn", minimum=1)
    number(sequence, "sequence", maximum=MAX_PACKETS - 1)
    if length != len(raw) - HEADER.size or not 0 < length <= MAX_PAYLOAD:
        raise ProtocolError("invalid payload length")
    return turn, sequence, raw[HEADER.size:]


class Recording:
    def __init__(self, turn):
        self.turn = number(turn, "turn", minimum=1)
        self.packets = []
        self.byte_count = 0
        self.crc = 0

    def append(self, sequence, payload):
        if sequence != len(self.packets) or len(self.packets) >= MAX_PACKETS:
            raise ProtocolError("audio sequence or limit")
        self.packets.append(payload)
        self.byte_count += len(payload)
        self.crc = zlib.crc32(payload, self.crc)

    def finish(self, message):
        samples = number(message.get("samples"), "samples", MAX_SAMPLES, MIN_SAMPLES)
        delay = number(message.get("delay"), "delay", FRAME_SAMPLES - 1)
        frames = number(message.get("frames"), "frames", MAX_PACKETS, 1)
        byte_count = number(message.get("bytes"), "bytes", MAX_PACKETS * MAX_PAYLOAD, 1)
        crc = number(message.get("crc32"), "crc32")
        if frames != (samples + delay + FRAME_SAMPLES - 1) // FRAME_SAMPLES:
            raise ProtocolError("sample/frame count mismatch")
        if (frames, byte_count, crc) != (len(self.packets), self.byte_count, self.crc):
            raise ProtocolError("audio integrity mismatch")
        return {"turn": self.turn, "samples": samples, "delay": delay, "frames": frames,
                "bytes": byte_count, "crc32": crc}

    def clear(self):
        self.packets.clear()
        self.byte_count = self.crc = 0
