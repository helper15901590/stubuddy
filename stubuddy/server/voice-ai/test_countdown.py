"""Countdown policy and real local WebSocket/Opus tests, no credentials/APIs."""
import asyncio
import json
import unittest
from unittest import mock
from websockets.asyncio.client import connect
import base64
import zlib
from providers import clean_spoken_text

import countdown_tools as tools
from voice_server import VoiceService
from countdown_smoke import exercise
import test_voice_ai as baseline


def plan(**values):
    return {'tool_calls': [{'function': {'name': 'countdown_plan', 'arguments': json.dumps(values)}}]}


def status(active=False, remaining=0, volume=50, ok=True):
    return {'ok': ok, 'active': active, 'remaining': remaining, 'alarm_volume': volume}


class PolicyTests(unittest.IsolatedAsyncioTestCase):
    def test_log_reproductions_route_to_real_device_including_asr_itn(self):
        for query in ('设置一分钟后，喝水。', '设置1分钟后喝水', '设定一分钟后喝水',
                      '提醒我30分钟后做眼保健操', '提醒我半小时后休息', '30s后提醒我喝水。',
                      '30 S后提醒我喝水', '设置0.5min后喝水', '设置1h后休息', '我现在有哪些提醒？'):
            self.assertTrue(tools.intent(query), query)
        messages = [{'content': '设置一分钟后喝水'}, {'content': '倒计时开始啦'}, {'content': '还有多少秒？'}]
        self.assertTrue(tools.intent('还有多少秒？', messages))
        self.assertFalse(tools.intent('一分钟有多少秒？'))
        self.assertFalse(tools.intent('一分钟后会下雨吗？'))
        self.assertFalse(tools.intent('再加1', messages))

    def test_explicit_duration_parser_preserves_units_half_compounds_and_bounds(self):
        examples = {'设置一分钟后喝水': 60, '30s后提醒我喝水': 30, '设置0.5min后喝水': 30,
                    '半小时后提醒我': 1800, '一个半小时后提醒我': 5400,
                    '一小时二十分钟后提醒我': 4800, '一分半后提醒我': 90,
                    '设置二十四小时倒计时': 86400, '设置24小时1秒倒计时': 86401,
                    '设置负一分钟倒计时': -60, '提醒我一点五分钟后喝水': 90,
                    '30秒后做一分钟操': 30}
        for query, expected in examples.items():
            self.assertEqual(tools.duration_seconds(query), expected, query)

    async def test_too_short_explicit_input_cannot_be_accepted_or_clamped_by_model(self):
        for query in ('30s后提醒我喝水', '设置30秒后喝水', '设置零点五分钟倒计时', '设置25小时后喝水'):
            provider = mock.Mock(settings={})
            provider.completion_message = mock.AsyncMock(return_value=plan(seconds=60, text='时间到啦'))
            execute = mock.AsyncMock(return_value=status())
            prepare = mock.AsyncMock()
            answer = await tools.answer(provider, [{'content': query}], execute, prepare)
            self.assertEqual(answer, tools.RANGE_REPLY)
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
            provider.completion_message.assert_not_called()
            prepare.assert_not_called()

    async def test_current_explicit_duration_beats_wrong_model_plan_and_directions_are_removed(self):
        provider = mock.Mock(settings={})
        provider.completion_message = mock.AsyncMock(return_value=plan(seconds=30, text='（轻快地）喝水时间到啦！'))
        execute = mock.AsyncMock(side_effect=[status(), status(True, 60)])
        prepare = mock.AsyncMock()
        answer = await tools.answer(provider, [{'content': '设置一分钟后喝水'}], execute, prepare)
        self.assertIn('开始啦', answer)
        self.assertEqual(prepare.await_args.args[0], '喝水时间到啦！')
        self.assertEqual(execute.await_args_list[-1].args[1]['seconds'], 60)
        self.assertEqual(execute.await_args_list[-1].args[1]['text'], '喝水时间到啦！')

    def test_spoken_prose_strips_nested_directions_and_preserves_math(self):
        self.assertEqual(clean_spoken_text('（轻快地）你好！（微笑）一起学习吧。'), '你好！一起学习吧。')
        self.assertEqual(clean_spoken_text('(cheerfully)你好（温柔地（点头））呀。'), '你好呀。')
        self.assertEqual(clean_spoken_text('你好。（轻快地'), '你好。')
        self.assertEqual(clean_spoken_text('（2+3）×4等于20。'), '（2+3）×4等于20。')
    async def test_bounds_and_no_unconfirmed_start(self):
        for seconds in (0, 30, 59, 86401, True):
            provider = mock.Mock(settings={})
            provider.completion_message = mock.AsyncMock(return_value=plan(seconds=seconds, text='time'))
            execute = mock.AsyncMock(return_value=status())
            prepare = mock.AsyncMock()
            answer = await tools.answer(provider, [{'role': 'user', 'content': '设置倒计时'}], execute, prepare)
            self.assertIn('一分钟到二十四小时', answer)
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
            prepare.assert_not_called()
        execute = mock.AsyncMock(return_value=status(ok=False))
        with self.assertRaises(ValueError):
            await tools.answer(provider, [{'role': 'user', 'content': '设置倒计时'}], execute, prepare)

    async def test_one_active_query_and_cancel(self):
        provider = mock.Mock()
        execute = mock.AsyncMock(return_value=status(True, 70))
        prepare = mock.AsyncMock()
        answer = await tools.answer(provider, [{'content': '设置两分钟倒计时'}], execute, prepare)
        self.assertIn('先取消', answer)
        prepare.assert_not_called()
        answer = await tools.answer(provider, [{'content': '倒计时还剩多久'}], execute, prepare)
        self.assertIn('一分', answer)
        execute = mock.AsyncMock(side_effect=[status(True, 70), status()])
        self.assertEqual(await tools.answer(provider, [{'content': '取消倒计时'}], execute, prepare), '倒计时已取消。')

    async def test_cache_before_start_and_24_hour_boundary(self):
        provider = mock.Mock(settings={})
        provider.completion_message = mock.AsyncMock(return_value=plan(seconds=86400, text='时间到啦，来休息一下吧。'))
        order = []
        async def execute(name, values):
            order.append(name)
            return status(name == tools.START, 86400 if name == tools.START else 0)
        async def prepare(text, identity):
            self.assertEqual(len(identity), 16)
            order.append('voice')
        answer = await tools.answer(provider, [{'content': '设置二十四小时倒计时'}], execute, prepare)
        self.assertEqual(order, [tools.GET, 'voice', tools.START])
        self.assertIn('二十四小时', answer)
        async def fail_prepare(text, identity): raise ValueError('fake TTS failure')
        order.clear()
        with self.assertRaises(ValueError):
            await tools.answer(provider, [{'content': '设置倒计时'}], execute, fail_prepare)
        self.assertEqual(order, [tools.GET])

    async def test_alarm_volume_is_independent_and_uses_actual_ack(self):
        provider = mock.Mock(settings={})
        provider.completion_message = mock.AsyncMock(return_value=plan(volume=60))
        execute = mock.AsyncMock(side_effect=[status(volume=50), status(volume=60)])
        answer = await tools.answer(provider, [{'content': '提醒音量调到60'}], execute, mock.AsyncMock())
        self.assertIn('百分之六十', answer)
        self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {}), mock.call(tools.VOLUME, {'volume': 60})])
        execute = mock.AsyncMock(side_effect=[status(), status(volume=40)])
        with self.assertRaises(ValueError):
            await tools.answer(provider, [{'content': '提醒音量调到60'}], execute, mock.AsyncMock())

    async def test_short_alarm_volume_setting_and_countdown_query_use_latest_topic(self):
        messages = [{'role': 'user', 'content': '提醒音量多少'},
                    {'role': 'assistant', 'content': '当前提醒音量是百分之五十。'},
                    {'role': 'user', 'content': '调到60'}]
        self.assertTrue(tools.intent('调到60', messages))
        provider = mock.Mock(settings={})
        provider.completion_message = mock.AsyncMock(return_value=plan(volume=60))
        execute = mock.AsyncMock(side_effect=[status(), status(volume=60)])
        self.assertIn('提醒音量', await tools.answer(provider, messages, execute, mock.AsyncMock()))
        self.assertFalse(tools.intent('再加1', messages[:-1] + [{'role': 'user', 'content': '再加1'}]))
        timer_messages = [{'role': 'user', 'content': '设置倒计时'},
                          {'role': 'assistant', 'content': '倒计时开始啦。'},
                          {'role': 'user', 'content': '还有多久'}]
        self.assertTrue(tools.intent('还有多久', timer_messages))
        self.assertFalse(tools.intent('还有多久', [{'role': 'user', 'content': '还有多久'}]))

    def test_intent_does_not_steal_normal_volume_math_or_calendar(self):
        for query in ('设置一分钟倒计时', '五分钟后提醒我喝水', '提醒我五分钟后喝水', '提醒音量多少', '取消倒计时'):
            self.assertTrue(tools.intent(query))
        for query in ('音量调到50', '1+1', '再加1', '明天八点提醒我'):
            self.assertFalse(tools.intent(query))


@unittest.skipUnless(baseline.NATIVE_OPUS, 'Native libopus required')
class IntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def test_chat_reply_tts_and_history_share_clean_spoken_text(self):
        provider = mock.Mock()
        provider.recognize = mock.AsyncMock(return_value='你好，请鼓励我学习。')
        provider.answer = mock.AsyncMock(return_value='（轻快地）你好！（微笑）一起加油吧。')
        provider.synthesize = mock.AsyncMock(return_value=bytes(9600))
        service = VoiceService({'device_token': 'a' * 64, 'prompt': 'private custom prompt'},
                               provider=provider, codec=(lambda *args: bytes(9600), baseline.fake_encode))
        self.assertIn('不写括号', service.prompt)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f'ws://127.0.0.1:{port}/stubuddy/ai', additional_headers={
                    'Authorization': 'Bearer ' + 'a' * 64, 'Device-Id': '02:00:00:00:04:33',
                    'Protocol-Version': '1'}) as socket:
                await socket.send(json.dumps({'type': 'hello', 'transport': 'websocket', 'ai_protocol': 1,
                    'audio_params': baseline.AUDIO_PARAMS}))
                await socket.recv()
                await socket.send('{"type":"listen","state":"start","mode":"manual","turn":1}')
                packet = b'uploaded-opus'
                crc = 0
                for index in range(5):
                    crc = zlib.crc32(packet, crc)
                    await socket.send(baseline.pack(1, index, packet))
                await socket.send(json.dumps({'type': 'listen', 'state': 'stop', 'turn': 1,
                    'samples': 4800, 'delay': 0, 'frames': 5, 'bytes': 5 * len(packet), 'crc32': crc}))
                reply_text = None
                while True:
                    raw = await asyncio.wait_for(socket.recv(), 5)
                    if not isinstance(raw, str): continue
                    message = json.loads(raw)
                    if message['type'] == 'reply_text': reply_text = message['text']
                    if message['type'] == 'reply_end': break
                self.assertEqual(reply_text, '你好！一起加油吧。')
                provider.synthesize.assert_awaited_once_with(reply_text)
                self.assertEqual(service.histories['02:00:00:00:04:33'].pairs[-1][1], reply_text)

    async def test_reboot_cache_sync_needs_no_conversation_turn_and_adds_no_history(self):
        from audio import decode
        provider = mock.Mock()
        provider.synthesize = mock.AsyncMock(return_value=baseline.sine_pcm(16000))
        service = VoiceService({'device_token': 'a' * 64}, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f'ws://127.0.0.1:{port}/stubuddy/ai', additional_headers={
                    'Authorization': 'Bearer ' + 'a' * 64, 'Device-Id': '02:00:00:00:04:32',
                    'Protocol-Version': '1'}) as socket:
                await socket.send(json.dumps({'type': 'hello', 'transport': 'websocket', 'ai_protocol': 1,
                    'reply_stream': 1, 'max_reply_packet_bytes': 1275, 'device_controls': 1,
                    'volume_restore': 1, 'countdown': 1, 'audio_params': baseline.AUDIO_PARAMS}))
                hello = json.loads(await socket.recv())
                await socket.send(json.dumps({'type': 'countdown_sync', 'id': 'a' * 16, 'text': '喝水时间到啦。'}))
                packets = []
                while True:
                    message = json.loads(await asyncio.wait_for(socket.recv(), 5))
                    self.assertEqual(message['session_id'], hello['session_id'])
                    if message['type'] == 'countdown_packet':
                        self.assertEqual(message['sequence'], len(packets))
                        packets.append(base64.b64decode(message['data'], validate=True))
                    if message.get('state') == 'end':
                        self.assertEqual(zlib.crc32(b''.join(packets)), message['crc32'])
                        self.assertEqual(len(decode(packets, message['samples'], message['delay'])), 32000)
                        break
                self.assertEqual(len(service.histories['02:00:00:00:04:32'].pairs), 0)
                await socket.send('{"type":"ping"}')
                self.assertEqual(json.loads(await socket.recv())['type'], 'pong')

    async def test_complete_ai1_cache_and_tools_without_changing_history_rule(self):
        queries = iter(('设置一分钟后，喝水。', '还有多少秒？', '提醒音量调到百分之六十。', '取消倒计时。',
                        '30s后提醒我喝水。', '现在有什么提醒？'))
        class Provider:
            settings = {}
            async def synthesize(self, text): return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return next(queries)
            async def completion_message(self, body):
                if '音量' in body['messages'][-1]['content']: return plan(volume=60)
                return plan(seconds=60, text='（轻快地）喝水时间到啦，喝两口水吧。')
            async def synthesize_stream(self, text):
                yield baseline.sine_pcm(16000)
        settings = {'device_token': 'a' * 64}
        provider = Provider()
        service = VoiceService(settings, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            await exercise(settings, provider, f'ws://127.0.0.1:{port}/stubuddy/ai')
            history = service.histories['02:00:00:00:04:31']
            self.assertEqual(len(history.pairs), 6)
            self.assertIn('倒计时已取消', history.pairs[3][1])
            self.assertIn('一分钟到二十四小时', history.pairs[4][1])

    async def test_prepared_voice_enforces_twelve_second_limit(self):
        provider = mock.Mock()
        provider.synthesize = mock.AsyncMock(return_value=bytes(384002))
        send = mock.AsyncMock()
        with self.assertRaises(ValueError):
            await tools.prepare_voice(provider, 'time', 'a' * 16, send)
        send.assert_not_called()
