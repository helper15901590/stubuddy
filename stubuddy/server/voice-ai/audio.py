"""Mature libopus bindings; per-turn state, measured codec lookahead and exact PCM length."""
import io
import wave

RATE = 16000
FRAME = 960
MAX_SAMPLES = 960000
REPLY_PACKET_LIMIT = 1275


class StreamEncoder:
    """One continuous Opus state; bounded input tail and a hard packet ceiling."""
    def __init__(self, packet_limit=REPLY_PACKET_LIMIT):
        import opuslib_next
        if type(packet_limit) is not int or not 64 <= packet_limit <= REPLY_PACKET_LIMIT:
            raise ValueError("Invalid negotiated reply packet limit")
        self.encoder = opuslib_next.Encoder(RATE, 1, opuslib_next.APPLICATION_AUDIO)
        self.encoder.bitrate = 24000
        self.encoder.complexity = 5
        self.limit = packet_limit
        self.delay = self.encoder.lookahead
        if not 0 <= self.delay < FRAME:
            raise ValueError("Invalid libopus lookahead")
        self.tail = bytearray()
        self.byte_count = 0
        self.frames = 0
        self.closed = False

    @property
    def samples(self):
        return self.byte_count // 2

    def packet(self, pcm):
        import opuslib_next.api.encoder
        # libopus's public max_data_bytes parameter bounds VBR peaks as well
        # as average bitrate. Do not truncate or discard compressed packets.
        result = opuslib_next.api.encoder.encode(
            self.encoder.encoder_state, pcm, FRAME, self.limit)
        if not 0 < len(result) <= self.limit:
            raise ValueError("Opus reply packet limit exceeded")
        self.frames += 1
        return result

    def feed(self, pcm):
        if self.closed or not isinstance(pcm, bytes) or self.byte_count + len(pcm) > MAX_SAMPLES * 2:
            raise ValueError("Invalid or oversized streaming PCM")
        self.byte_count += len(pcm)
        self.tail.extend(pcm)
        packets = []
        while len(self.tail) >= FRAME * 2:
            packets.append(self.packet(bytes(self.tail[:FRAME * 2])))
            del self.tail[:FRAME * 2]
        return packets

    def finish(self):
        if self.closed or not self.byte_count or self.byte_count % 2:
            raise ValueError("Invalid final streaming PCM")
        packets = self.feed(bytes(max(0, 9600 - self.byte_count)))
        self.closed = True
        needed = (self.samples + self.delay + FRAME - 1) // FRAME
        while self.frames < needed:
            packets.append(self.packet(bytes(self.tail).ljust(FRAME * 2, b"\0")))
            self.tail.clear()
        return packets


def decode(packets, samples, delay):
    import opuslib_next
    decoder = opuslib_next.Decoder(RATE, 1)
    pcm = b"".join(decoder.decode(packet, FRAME) for packet in packets)
    pcm = pcm[delay * 2:(delay + samples) * 2]
    if len(pcm) != samples * 2:
        raise ValueError("Decoded sample count mismatch")
    return pcm


def wav_bytes(pcm):
    stream = io.BytesIO()
    with wave.open(stream, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm)
    return stream.getvalue()


def encode(pcm):
    import opuslib_next
    if not pcm or len(pcm) % 2 or len(pcm) > MAX_SAMPLES * 2:
        raise ValueError("TTS PCM must contain 1..60 seconds of mono PCM16")
    encoder = opuslib_next.Encoder(RATE, 1, opuslib_next.APPLICATION_AUDIO)
    encoder.bitrate = 24000
    encoder.complexity = 5
    delay = encoder.lookahead
    if not 0 <= delay < FRAME:
        raise ValueError("Invalid libopus lookahead")
    samples = len(pcm) // 2
    count = (samples + delay + FRAME - 1) // FRAME
    padded = pcm + bytes(count * FRAME * 2 - len(pcm))
    # Transitional buffered ai/1 clients still advertise no independent
    # downlink capacity; respect their accepted 227-byte storage until flashed.
    packets = [opuslib_next.api.encoder.encode(encoder.encoder_state,
               padded[offset:offset + FRAME * 2], FRAME, 227)
               for offset in range(0, len(padded), FRAME * 2)]
    return packets, samples, delay
