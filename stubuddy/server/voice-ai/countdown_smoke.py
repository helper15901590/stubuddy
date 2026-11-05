"""Real provider / deployed ai/1 smoke, using a simulated countdown device."""
import argparse
import asyncio
import base64
import json
from pathlib import Path
import sys
import zlib

from websockets.asyncio.client import connect
from audio import encode, decode
from providers import DashScope
import countdown_tools as tools

sys.path.append(str(Path(__file__).resolve().parent.parent / 'ws-echo'))
from echo_protocol import AUDIO_PARAMS, Recording, pack, unpack


async def exercise(settings, provider, url='ws://127.0.0.1:8766/stubuddy/ai'):
    headers = {'Authorization': 'Bearer ' + settings['device_token'],
               'Device-Id': '02:00:00:00:04:31', 'Protocol-Version': '1'}
    active, volume = False, 50
    cached = None
    cache_packets = []
    async with connect(url, additional_headers=headers, compression=None, max_size=4112) as socket:
        await socket.send(json.dumps({'type': 'hello', 'transport': 'websocket', 'ai_protocol': 1,
            'reply_stream': 1, 'max_reply_packet_bytes': 1275, 'device_controls': 1,
            'volume_restore': 1, 'countdown': 1, 'audio_params': AUDIO_PARAMS}))
        hello = json.loads(await socket.recv())
        if hello.get('countdown') != 1:
            raise ValueError('Missing countdown negotiation')
        for turn, query in enumerate(('设置一分钟后喝水。', '还有多少秒？',
                                      '提醒音量调到百分之六十。', '取消倒计时。',
                                      '三十秒后提醒我喝水。', '现在有什么提醒？'), 1):
            async with asyncio.timeout(95):
                pcm = await provider.synthesize(query)
                packets, samples, delay = encode(pcm.ljust(9600, b'\0'))
                await socket.send(json.dumps({'type': 'listen', 'state': 'start', 'mode': 'manual', 'turn': turn}))
                crc = 0
                for sequence, packet in enumerate(packets):
                    crc = zlib.crc32(packet, crc)
                    await socket.send(pack(turn, sequence, packet))
                await socket.send(json.dumps({'type': 'listen', 'state': 'stop', 'turn': turn,
                    'samples': samples, 'delay': delay, 'frames': len(packets),
                    'bytes': sum(map(len, packets)), 'crc32': crc}))
                reply = Recording(turn)
                answered = False
                operations = []
                answer_text = ''
                while True:
                    raw = await socket.recv()
                    if isinstance(raw, bytes):
                        identity, index, packet = unpack(raw)
                        if identity != turn:
                            raise ValueError('Stale reply')
                        reply.append(index, packet)
                        continue
                    message = json.loads(raw)
                    if message.get('session_id') != hello['session_id']:
                        raise ValueError('Invalid session')
                    kind = message['type']
                    if kind == 'countdown_cache':
                        if message['state'] == 'start':
                            cache_packets = []
                            cached = None
                        else:
                            if (len(cache_packets) != message['frames'] or
                                    zlib.crc32(b''.join(cache_packets)) != message['crc32'] or
                                    len(decode(cache_packets, message['samples'], message['delay'])) != message['samples'] * 2):
                                raise ValueError('Invalid prepared reminder voice')
                            cached = message['id']
                    elif kind == 'countdown_packet':
                        packet = base64.b64decode(message['data'], validate=True)
                        if message['sequence'] != len(cache_packets) or not 0 < len(packet) <= 227:
                            raise ValueError('Invalid cache packet')
                        cache_packets.append(packet)
                    elif kind == 'device_tool':
                        name = message['name']
                        operations.append(name)
                        if name == tools.START:
                            if active or message.get('seconds') != 60 or message.get('cache_id') != cached:
                                raise ValueError('Invalid timer start or start-before-cache')
                            if '(' in message['text'] or '（' in message['text']:
                                raise ValueError('Spoken reminder contains directions')
                            active = True
                        elif name == tools.CANCEL:
                            active = False
                        elif name == tools.VOLUME:
                            if message.get('volume') != 60:
                                raise ValueError('Incorrect alarm volume')
                            volume = 60
                        elif name != tools.GET:
                            raise ValueError('Unexpected device operation')
                        await socket.send(json.dumps({'type': 'tool_result', 'turn': turn,
                            'session_id': hello['session_id'], 'call_id': message['call_id'],
                            'ok': True, 'active': active, 'remaining': 60 if active else 0, 'alarm_volume': volume}))
                    elif kind == 'reply_text':
                        answered = bool(message.get('text'))
                        answer_text = message.get('text', '')
                        if '(' in answer_text or '（' in answer_text:
                            raise ValueError('Spoken reply contains directions')
                    elif kind == 'reply_chunk':
                        if len(reply.packets) != message['frames'] or reply.crc != message['crc32']:
                            raise ValueError('Invalid reply checkpoint')
                    elif kind == 'reply_end':
                        reply.finish(message)
                        decode(reply.packets, message['samples'], message['delay'])
                        if not answered:
                            raise ValueError('Missing answer')
                        await socket.send(json.dumps({'type': 'abort', 'turn': turn}))
                        break
                    elif kind in ('turn_error', 'turn_empty'):
                        raise ValueError('Countdown provider chain failed')
                if turn == 1 and not active or turn == 3 and volume != 60 or turn == 4 and active:
                    raise ValueError('Missing real device receipts')
                if turn in (5, 6) and (active or operations != [tools.GET]):
                    raise ValueError('Unsupported countdown changed device state')
                if turn == 5 and '一分钟到二十四小时' not in answer_text:
                    raise ValueError('Thirty-second countdown was not explicitly refused')
                print(f'PASS: countdown simulated-device turn {turn}; reply Opus/CRC/samples verified')
    print('PASS: real ASR/LLM/TTS oral countdown create/query/alarm-volume/cancel, 30-second refusal and clean prepared voice')
    print('Physical deadlines, NVS, priority, button and LED still require hardware testing.')


async def main(settings):
    provider = DashScope(settings)
    try:
        await exercise(settings, provider)
    finally:
        await provider.aclose()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('.private.json'))
    args = parser.parse_args()
    try:
        asyncio.run(main(json.loads(args.config.read_text(encoding='utf-8'))))
    except KeyboardInterrupt:
        pass
    except Exception as error:
        raise SystemExit('FAIL: countdown smoke (' + type(error).__name__ + '); no physical timer was created') from None
