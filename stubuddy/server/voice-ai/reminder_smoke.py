"""Real ASR/LLM/TTS and WSS reminder smoke; simulated device only."""
import argparse
import asyncio
import base64
from datetime import datetime, timedelta
import json
from pathlib import Path
import sys
import time
import zlib

from websockets.asyncio.client import connect
from audio import encode, decode
from providers import DashScope
import reminder_tools as tools
import clock_answers
sys.path.append(str(Path(__file__).resolve().parent.parent / 'ws-echo'))
from echo_protocol import AUDIO_PARAMS, Recording, pack, unpack


class ClockCheckFailure(ValueError):
    def __init__(self, code):
        if code not in ('asr_clock_intent_mismatch', 'clock_answer_mismatch', 'clock_source_missing'):
            raise ValueError('Invalid smoke diagnostic code')
        self.code = code
        super().__init__(code)


def verify_clock(query, recognized, answer, timestamp):
    wanted = clock_answers.intent(query)
    heard = clock_answers.intent(recognized) if isinstance(recognized, str) else None
    if not wanted or not heard or wanted[:2] != heard[:2]:
        raise ClockCheckFailure('asr_clock_intent_mismatch')
    if timestamp is None:
        raise ClockCheckFailure('clock_source_missing')
    if answer != clock_answers.answer(wanted, timestamp):
        raise ClockCheckFailure('clock_answer_mismatch')


class Device:
    def __init__(self):
        self.records = []
        self.volume = 50
        self.cached = None

    def apply(self, name, args):
        code = 0
        now = int(time.time())
        self.last_now = now
        if name in (tools.START, tools.TIMER):
            timer = name == tools.TIMER
            if args['cache_id'] != self.cached:
                raise ValueError('Device start before verified cache')
            if sum(row['kind'] == ('countdown' if timer else 'scheduled') for row in self.records) >= (1 if timer else 3):
                code = 1
            else:
                row = {'id': args['cache_id'], 'item': args['item'], 'text': args['text'],
                       'kind': 'countdown' if timer else 'scheduled',
                       'deadline': now + args['seconds'] if timer else args['deadline'],
                       'minute': args.get('minute', 0), 'weekdays': args.get('weekdays', 0)}
                if not timer and any(tools.conflict(row, old) for old in self.records if old['kind'] == 'scheduled'):
                    code = 2
                else:
                    self.records.append(row)
        elif name == tools.CANCEL:
            if not any(row['id'] == args['cache_id'] for row in self.records): code = 4
            self.records = [row for row in self.records if row['id'] != args['cache_id']]
        elif name == tools.VOLUME:
            self.volume = args['volume']
        elif name != tools.GET:
            raise ValueError('Unexpected tool')
        return {'ok': not code, 'error': code, 'now': now, 'alarm_volume': self.volume,
                'records': [dict(row) for row in self.records]}


def explicit_clock(when):
    # A clock below 13 needs a spoken period; otherwise daytime smoke requests
    # correctly trigger the production ambiguity guard instead of creating.
    period = '上午' if 1 <= when.hour < 12 else '中午' if when.hour == 12 else ''
    return f'{period}{when.hour}点{when.minute}分'


async def exercise(settings, provider, url='ws://127.0.0.1:8766/stubuddy/ai', queries=None, verify=None):
    now = datetime.now(tools.TZ)
    later = now + timedelta(minutes=15)
    weekly = now + timedelta(minutes=25)
    defaults = queries is None
    if defaults:
        queries = ['提醒我十分钟后喝水。',
            f'设置{later.year}年{later.month}月{later.day}日{explicit_clock(later)}提醒我阅读。',
            f'每周一二三四五六日的{explicit_clock(weekly)}提醒我整理书包。',
            '我有哪些提醒或倒计时？', '第一条还剩多久？', '提醒音量调到六十。',
            '取消第二条提醒。', '我有哪些提醒？', '三十秒后提醒我喝水。', '工作日七点提醒我起床。']
    device = Device()
    headers = {'Authorization': 'Bearer ' + settings['device_token'],
               'Device-Id': '02:00:00:00:04:42', 'Protocol-Version': '1'}
    async with connect(url, additional_headers=headers, compression=None, max_size=4112) as socket:
        await socket.send(json.dumps({'type': 'hello', 'transport': 'websocket', 'ai_protocol': 1,
            'reply_stream': 1, 'max_reply_packet_bytes': 1275, 'device_controls': 1,
            'volume_restore': 1, 'countdown': 1, 'scheduled': 1, 'audio_params': AUDIO_PARAMS}))
        hello = json.loads(await socket.recv())
        if hello.get('scheduled') != 1: raise ValueError('Missing reminder negotiation')
        answers = []
        operations = []
        cache_packets = []
        for turn, query in enumerate(queries, 1):
            async with asyncio.timeout(95):
                pcm = await provider.synthesize(query)
                packets, samples, delay = encode(pcm.ljust(9600, b'\0'))
                await socket.send(json.dumps({'type': 'listen', 'state': 'start', 'mode': 'manual', 'turn': turn}))
                crc = 0
                for index, packet in enumerate(packets):
                    crc = zlib.crc32(packet, crc)
                    await socket.send(pack(turn, index, packet))
                await socket.send(json.dumps({'type': 'listen', 'state': 'stop', 'turn': turn,
                    'samples': samples, 'delay': delay, 'frames': len(packets),
                    'bytes': sum(map(len, packets)), 'crc32': crc}))
                reply = Recording(turn)
                answer = ''
                device.last_stt = None
                calls, cache_starts = [], 0
                while True:
                    raw = await socket.recv()
                    if isinstance(raw, bytes):
                        identity, sequence, packet = unpack(raw)
                        if identity != turn: raise ValueError('Stale audio')
                        reply.append(sequence, packet)
                        continue
                    message = json.loads(raw)
                    if message.get('session_id') != hello['session_id']: raise ValueError('Session mismatch')
                    kind = message['type']
                    if kind == 'countdown_cache':
                        if message['state'] == 'start':
                            cache_packets = []
                            cache_starts += 1
                        else:
                            if (message['frames'] != len(cache_packets) or
                                    zlib.crc32(b''.join(cache_packets)) != message['crc32'] or
                                    len(decode(cache_packets, message['samples'], message['delay'])) != message['samples'] * 2):
                                raise ValueError('Reminder cache integrity')
                            device.cached = message['id']
                    elif kind == 'countdown_packet':
                        packet = base64.b64decode(message['data'], validate=True)
                        if message['sequence'] != len(cache_packets) or not 0 < len(packet) <= 227:
                            raise ValueError('Reminder packet sequence/size')
                        cache_packets.append(packet)
                    elif kind == 'device_tool':
                        calls.append(message['name'])
                        status = device.apply(message['name'], message)
                        await socket.send(json.dumps({'type': 'tool_result', 'turn': turn,
                            'call_id': message['call_id'], 'session_id': hello['session_id'], **status}))
                    elif kind == 'stt': device.last_stt = message.get('text')
                    elif kind == 'reply_text': answer = message['text']
                    elif kind == 'reply_chunk':
                        if len(reply.packets) != message['frames'] or reply.crc != message['crc32']:
                            raise ValueError('Reply checkpoint')
                    elif kind == 'reply_end':
                        reply.finish(message)
                        decode(reply.packets, message['samples'], message['delay'])
                        if not answer: raise ValueError('Missing answer')
                        await socket.send(json.dumps({'type': 'abort', 'turn': turn}))
                        break
                    elif kind in ('turn_error', 'turn_empty'):
                        raise ValueError('Reminder provider chain')
                answers.append(answer)
                operations.append(calls)
                if verify:
                    try:
                        verify(turn, answer, device, calls, cache_starts)
                    except Exception:
                        # Only synthetic test conversation and local tool names;
                        # never print profiles, HTTP bodies, headers or errors.
                        print(f'CHECK FAIL: turn={turn}; input={query[:180]!r}; '
                              f'ASR={(device.last_stt or "")[:180]!r}; answer={answer[:180]!r}; '
                              f'tools={calls!r}; cache_starts={cache_starts}', flush=True)
                        raise
                print(f'PASS: reminder simulated-device turn {turn}; reply/cache Opus/CRC/sample integrity')
        if defaults and (len(device.records) != 2 or device.volume != 60):
            raise ValueError('Reminder state/volume mismatch')
        if defaults and ('第一条' not in answers[3] or '喝水' not in answers[3] or '阅读' not in answers[3] or
                '剩' not in answers[4] or '一分钟到二十四小时' not in answers[8] or
                '星期' not in answers[9]):
            raise ValueError('Reminder list/remaining/refusal semantics')
    print('PASS: real ASR/LLM/TTS reminder create/list/remaining/volume/cancel/range/refusal')
    print('Physical deadlines, NVS, 3-minute limit, collision, LED and PTT still require hardware testing.')
    return device, answers, operations


ROUTING_QUERIES = ['你有哪些功能或工具？', '每周一、周三、周五，提醒我看书。', '晚上八点。',
    '我现在有几条提醒。', '我现在有多少条提醒？', '我有哪些提醒或倒计时？', '删除第一条。',
    '继续说。', '全部删除。', '每周，上午八点提醒我喝水。', '每周一到周五。',
    '我有哪些提醒？', '删除第一条。', '现在还有几条提醒？', '三十秒后提醒我喝水。',
    '每周日零点提醒我整理书包。', '我有哪些提醒？', '删除第一条。']


def verify_routing(turn, answer, device, calls, caches):
    expected_counts = [0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0]
    expected_calls = ([tools.GET, tools.START] if turn in (3, 11, 16) else
                      [tools.GET, tools.CANCEL] if turn in (7, 13, 18) else
                      [] if turn == 1 else [tools.GET])
    if (calls != expected_calls or caches != int(turn in (3, 11, 16)) or
            len(device.records) != expected_counts[turn - 1]):
        raise ValueError('Query/write isolation or short-command routing failed')
    if turn == 2 and '几点' not in answer: raise ValueError('Missing-time clarification')
    if turn == 3 and (device.records[0]['minute'] != 1200 or device.records[0]['weekdays'] != 21):
        raise ValueError('Pending clock/weekdays mismatch')
    if turn == 11 and (device.records[0]['minute'] != 480 or device.records[0]['weekdays'] != 31):
        raise ValueError('Pending weekday continuation mismatch')
    if turn == 16 and device.records[0]['minute'] != 0: raise ValueError('Explicit midnight rejected')
    if turn in (4, 5) and '一条' not in answer: raise ValueError('Count semantics')
    if turn in (8, 14) and '没有提醒' not in answer: raise ValueError('Stale reminder state')


async def routing_exercise(settings, provider, url='ws://127.0.0.1:8766/stubuddy/ai'):
    result = await exercise(settings, provider, url, ROUTING_QUERIES, verify_routing)
    print('PASS: missing-time clarification, explicit midnight, read-only counts, real short cancellation and capability help')
    return result


async def time_query_exercise(settings, provider, url='ws://127.0.0.1:8766/stubuddy/ai'):
    later = datetime.now(tools.TZ) + timedelta(minutes=15)
    queries = ['现在几点？', '请告诉我今天的日期是什么？', '今天星期几？', '请告诉我明天几月几号？', '十分钟后提醒我休息。',
        '定时提醒是什么内容？', f'设置{later.year}年{later.month}月{later.day}日{explicit_clock(later)}提醒我看书。',
        '定时提醒是什么内容？', '我现在有多少提成？', '是的。', '我现在有多少提成？', '不是。',
        '我有哪些提醒？', '取消第一条。', '提醒事项是什么？', '我现在有几条提醒？',
        '每天提醒我吃饭。', '每周一提醒我吃饭。', '星期一提醒我吃饭。',
        '明天提醒我。', '明天晚上七点提醒我。', '取消设置。', '现在时间是多少？',
        '设置一个倒计时。', '三十秒钟之后。', '吃饭。', '一分钟之后。', '取消倒计时。',
        '明天晚上七点提醒我。', '那是。', '取消设置。',
        '倒计时一分钟。', '取消倒计时。', '设置一个倒计时。', '一分钟之后。', '取消倒计时。']
    def verify(turn, answer, device, calls, caches):
        if turn >= 32:
            creating = turn in (32, 35)
            expected = [tools.GET, tools.TIMER] if creating else [tools.GET] if turn == 34 else [tools.GET, tools.CANCEL]
            if calls != expected or caches != int(creating) or len(device.records) != (2 if creating else 1):
                raise ValueError('Generic countdown operation mismatch')
            if creating and not any(row['kind'] == 'countdown' and row['item'] == '倒计时' and row['text'] == '时间到啦。' for row in device.records):
                raise ValueError('Generic countdown voice mismatch')
            if turn == 34 and '多久' not in answer: raise ValueError('Missing duration')
            return
        if turn >= 24:
            expected = [tools.GET, tools.TIMER] if turn == 27 else [tools.GET, tools.CANCEL] if turn == 28 else [] if turn == 31 else [tools.GET]
            if calls != expected or caches != int(turn == 27) or len(device.records) != (2 if turn == 27 else 1):
                raise ValueError('Countdown fragment escaped guarded task')
            if turn in (24, 26) and '多久' not in answer: raise ValueError('Missing duration prompt')
            if turn == 25 and '一分钟到二十四小时' not in answer: raise ValueError('Short countdown accepted')
            if turn == 27 and not any(row['kind'] == 'countdown' and row['item'] == '吃饭' for row in device.records):
                raise ValueError('Countdown subject lost')
            if turn == 29 and '做什么' not in answer: raise ValueError('Missing subject prompt')
            if turn == 30 and '没有听清' not in answer: raise ValueError('Vague subject accepted')
            return
        if turn >= 17:
            expected = [] if turn == 22 else [tools.GET]
            if calls != expected or caches or len(device.records) != 1:
                raise ValueError('Calendar clarification changed reminder state')
            if turn in (17, 18, 19, 20) and '几点' not in answer:
                raise ValueError('Calendar missing-clock clarification')
            if turn == 21 and '做什么' not in answer:
                raise ValueError('Missing reminder subject clarification')
            if turn == 23:
                verify_clock(queries[turn - 1], device.last_stt, answer, getattr(device, 'last_now', None))
            return
        if turn <= 4:
            verify_clock(queries[turn - 1], device.last_stt, answer, getattr(device, 'last_now', None))
        expected = ([tools.GET, tools.TIMER] if turn == 5 else [tools.GET, tools.START] if turn == 7 else
                    [tools.GET, tools.CANCEL] if turn == 14 else [] if turn in (9, 11, 12) else [tools.GET])
        if calls != expected or caches != int(turn in (5, 7)):
            raise ValueError('Clock/query operation isolation')
        if turn in (8, 15) and '看书' not in answer: raise ValueError('Missing reminder subject')
        if turn in (9, 11) and '提醒吗' not in answer: raise ValueError('ASR typo was not clarified')
        if turn == 10 and '二条' not in answer and '两条' not in answer: raise ValueError('Confirmation count')
        if turn == 12 and '重新说' not in answer: raise ValueError('Declined clarification')
        if len(device.records) != (0 if turn < 5 else 1 if turn < 7 or turn >= 14 else 2):
            raise ValueError('Query changed reminder state')
    result = await exercise(settings, provider, url, queries, verify)
    print('PASS: real clock/date/weekday, reminder subject and contextual typo confirmation; queries are read-only')
    return result


async def main(settings, routing=False, time_queries=False):
    provider = DashScope(settings)
    try: await (time_query_exercise(settings, provider) if time_queries else routing_exercise(settings, provider) if routing else exercise(settings, provider))
    finally: await provider.aclose()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('.private.json'))
    parser.add_argument('--routing', action='store_true')
    parser.add_argument('--time-queries', action='store_true')
    args = parser.parse_args()
    try: asyncio.run(main(json.loads(args.config.read_text(encoding='utf-8')), args.routing, args.time_queries))
    except KeyboardInterrupt: pass
    except Exception as error:
        code = error.code if isinstance(error, ClockCheckFailure) else type(error).__name__
        raise SystemExit('FAIL: reminder smoke (' + code + '); no physical reminder changed') from None
