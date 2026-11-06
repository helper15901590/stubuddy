"""Reminder semantics and actual local ai/1 integration. No keys or paid APIs."""
import asyncio
from datetime import datetime, timedelta
import json
import unittest
from unittest import mock
from websockets.asyncio.client import connect

import reminder_tools as tools
from reminder_smoke import Device, exercise
from voice_server import VoiceService
from test_countdown import plan
import test_voice_ai as baseline

NOW = int(datetime(2026, 11, 5, 12, 0, tzinfo=tools.TZ).timestamp())


def row(identity='a', kind='scheduled', minutes=10, mask=0, item='喝水'):
    deadline = NOW + minutes * 60
    return {'id': identity * 16, 'kind': kind, 'deadline': deadline,
            'minute': (deadline + 28800) % 86400 // 60 if kind == 'scheduled' else 0,
            'weekdays': mask, 'item': item, 'text': item + '时间到啦。'}


def status(records=None, ok=True, error=0):
    return {'ok': ok, 'error': error, 'now': NOW, 'alarm_volume': 50, 'records': records or []}


def provider_plan(**overrides):
    values = dict(kind='scheduled', date='2026-11-05', time='12:10', weekdays=[],
                  seconds=0, item='喝水', text='（轻快地）喝水时间到啦。', clarify='')
    values.update(overrides)
    provider = mock.Mock(settings={})
    provider.completion_message = mock.AsyncMock(return_value=plan(**values))
    return provider


class ReminderTests(unittest.IsolatedAsyncioTestCase):
    async def test_unified_list_reports_subject_time_order_and_rule(self):
        records = [row('a', minutes=20, mask=8, item='阅读'), row('b', 'countdown', 3)]
        execute = mock.AsyncMock(return_value=status(records))
        view = {}
        answer = await tools.answer(mock.Mock(), [{'content': '我有哪些提醒或倒计时？'}], execute, mock.AsyncMock(), view)
        self.assertIn('第一条，喝水', answer)
        self.assertIn('第二条，阅读，每周四', answer)
        self.assertNotIn('还剩', answer)
        self.assertEqual(view['ids'], ['b' * 16, 'a' * 16])

    async def test_remaining_uses_device_clock_and_disambiguates(self):
        execute = mock.AsyncMock(return_value=status([row()]))
        answer = await tools.answer(mock.Mock(), [{'content': '还剩多久？'}], execute, mock.Mock(), {})
        self.assertIn('十分钟', answer)
        execute.return_value = status([dict(row(), deadline=NOW + 12 * 86400 + 600)])
        answer = await tools.answer(mock.Mock(), [{'content': '还剩多久？'}], execute, mock.Mock(), {})
        self.assertIn('十二天十分钟', answer)
        execute.return_value = status([row(), row('b', minutes=30)])
        answer = await tools.answer(mock.Mock(), [{'content': '还剩多久？'}], execute, mock.Mock(), {})
        self.assertIn('哪一条', answer)

    async def test_numbered_cancel_binds_original_id_even_if_order_changes(self):
        a, b = row(), row('b', minutes=20, item='阅读')
        view = {}
        tools.listing(status([a, b]), view)
        a['deadline'] += 86400
        execute = mock.AsyncMock(side_effect=[status([b, a]), status([b])])
        answer = await tools.answer(mock.Mock(), [{'content': '取消第一条提醒'}], execute, mock.Mock(), view)
        self.assertIn('已取消', answer)
        self.assertEqual(execute.await_args.args, (tools.CANCEL, {'cache_id': a['id']}))

    async def test_stale_number_never_cancels_replacement(self):
        view = {'ids': ['a' * 16]}
        execute = mock.AsyncMock(return_value=status([row('b', item='阅读')]))
        answer = await tools.answer(mock.Mock(), [{'content': '取消第一条提醒'}], execute, mock.Mock(), view)
        self.assertIn('哪一条', answer)
        self.assertEqual(execute.await_count, 1)

    async def test_duplicate_subject_requires_selection(self):
        execute = mock.AsyncMock(return_value=status([row(), row('b', minutes=30)]))
        answer = await tools.answer(mock.Mock(), [{'content': '取消喝水提醒'}], execute, mock.Mock(), {})
        self.assertIn('哪一条', answer)
        self.assertEqual(execute.await_count, 1)

    async def test_create_cleans_directions_and_requires_real_receipt(self):
        provider = provider_plan()
        prepare = mock.AsyncMock()
        async def execute(name, args):
            if name == tools.GET: return status()
            self.assertEqual(prepare.await_count, 1)
            return status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'])])
        answer = await tools.answer(provider, [{'content': '今天中午十二点十分提醒我喝水'}], execute, prepare, {})
        self.assertIn('今天十二点十分', answer)
        self.assertEqual(prepare.await_args.args[0], '喝水时间到啦。')

    async def test_failed_device_save_does_not_claim_success(self):
        execute = mock.AsyncMock(side_effect=[status(), status(ok=False, error=5)])
        answer = await tools.answer(provider_plan(), [{'content': '今天中午十二点十分提醒我喝水'}], execute, mock.AsyncMock(), {})
        self.assertIn('保存失败', answer)

    async def test_wrong_device_rule_receipt_is_rejected(self):
        async def execute(name, args):
            return status() if name == tools.GET else status([dict(row(), id=args['cache_id'], minute=800)])
        with self.assertRaises(ValueError):
            await tools.answer(provider_plan(), [{'content': '今天中午十二点十分提醒我喝水'}], execute, mock.AsyncMock(), {})

    async def test_fourth_schedule_is_refused_without_tts(self):
        execute = mock.AsyncMock(return_value=status([row(), row('b', minutes=20), row('c', minutes=30)]))
        prepare = mock.AsyncMock()
        answer = await tools.answer(mock.Mock(), [{'content': '明天八点提醒我阅读'}], execute, prepare, {})
        self.assertIn('三个', answer)
        prepare.assert_not_called()

    async def test_second_countdown_and_thirty_seconds_are_refused(self):
        for query, records in [('三十秒后提醒我喝水', []), ('一分钟后提醒我喝水', [row(kind='countdown')])]:
            execute = mock.AsyncMock(return_value=status(records))
            prepare = mock.AsyncMock()
            await tools.answer(mock.Mock(), [{'content': query}], execute, prepare, {})
            prepare.assert_not_called()
            self.assertEqual(execute.await_count, 1)

    async def test_workdays_type_is_not_created(self):
        execute = mock.AsyncMock(return_value=status())
        prepare = mock.AsyncMock()
        answer = await tools.answer(mock.Mock(), [{'content': '工作日七点提醒我起床'}], execute, prepare, {})
        self.assertIn('指定星期', answer)
        prepare.assert_not_called()

    async def test_past_or_ambiguous_time_is_not_created(self):
        for provider in [provider_plan(time='11:59'), provider_plan(clarify='请问早上七点还是晚上七点？')]:
            execute, prepare = mock.AsyncMock(return_value=status()), mock.AsyncMock()
            await tools.answer(provider, [{'content': '七点提醒我喝水'}], execute, prepare, {})
            prepare.assert_not_called()
            self.assertEqual(execute.await_count, 1)

    def test_weekly_clock_dates_and_sunday_boundary(self):
        for query, mask in [('每周一、三、五晚上八点提醒我', 21), ('每周五到周一提醒我', 113),
                            ('每周一17点提醒我', 1), ('每周一二三四五六日提醒我', 127), ('每天提醒我', 127)]:
            self.assertEqual(tools.explicit_weekdays(query), mask)
        self.assertTrue(tools.intent('还剩多久？'))
        self.assertFalse(tools.intent('一百减三十还剩多少？'))
        for mask in range(1, 128):
            next_time = tools.next_weekly(30, mask, NOW)
            self.assertGreater(next_time, NOW)
            self.assertLessEqual(next_time, NOW + 7 * 86400)
            self.assertTrue(mask & 1 << datetime.fromtimestamp(next_time, tools.TZ).weekday())
        self.assertIn('明天', tools.time_words(NOW + 86400, NOW))
        self.assertIn('一分五秒', tools.time_words(NOW + 65, NOW))
        self.assertIn('1月1日', tools.time_words(int(datetime(2027, 1, 1, tzinfo=tools.TZ).timestamp()), NOW))

    def test_spacing_four_five_minutes_different_days_and_future_weeks(self):
        self.assertTrue(tools.conflict(row(), row('b', minutes=14)))
        self.assertFalse(tools.conflict(row(), row('b', minutes=15)))
        thu = row(mask=8)
        later = row('b', minutes=21 * 1440 + 14)
        self.assertTrue(tools.conflict(thu, later))
        later['deadline'] += 60
        self.assertFalse(tools.conflict(thu, later))
        sunday = dict(row(), minute=1438, weekdays=64)
        monday = dict(row('b'), minute=2, weekdays=1)
        self.assertTrue(tools.conflict(sunday, monday))
        monday['minute'] = 3
        self.assertFalse(tools.conflict(sunday, monday))

    async def test_cancelled_and_reboot_device_subjects_ignore_chat_history(self):
        execute = mock.AsyncMock(return_value=status([row(item='整理书包')]))
        messages = [{'content': '喝水提醒'}, {'content': '我有哪些提醒？'}]
        answer = await tools.answer(mock.Mock(), messages, execute, mock.Mock(), {})
        self.assertIn('整理书包', answer)
        self.assertNotIn('喝水', answer)
        execute.return_value = status()
        self.assertEqual(await tools.answer(mock.Mock(), messages, execute, mock.Mock(), {}), '现在没有提醒。')

    async def test_alarm_volume_is_acknowledged_independently(self):
        execute = mock.AsyncMock(side_effect=[status(), dict(status(), alarm_volume=60)])
        provider = mock.Mock(settings={})
        provider.completion_message = mock.AsyncMock(return_value=plan(volume=60))
        answer = await tools.answer(provider, [{'content': '提醒音量调到六十'}], execute, mock.Mock(), {})
        self.assertIn('百分之六十', answer)
        self.assertEqual(execute.await_args.args, (tools.VOLUME, {'volume': 60}))

    def test_invalid_status_and_duplicate_ids_are_rejected(self):
        for value in [status([row(), row()]), dict(status(), now=True), dict(status(), records=[dict(row(), item='')])]:
            with self.assertRaises(ValueError): tools.validate_status(value)

    async def test_real_local_websocket_ten_rounds_opus_history_and_abort(self):
        # Same packaged smoke used by the user, with deterministic upstreams.
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.last_input = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm):
                return self.last_input
            async def completion_message(self, body):
                query = body['messages'][-1]['content']
                if '音量' in query: return plan(volume=60)
                weekly = '每周' in query
                clock = __import__('re').search(r'(\d+)点(\d+)分', query)
                date = __import__('re').search(r'(\d{4})年(\d+)月(\d+)日', query)
                return plan(kind='scheduled' if clock else 'countdown', seconds=600,
                    date='-'.join(f'{int(x):02}' for x in date.groups()) if date else '',
                    time=f'{int(clock[1]):02}:{int(clock[2]):02}' if clock else '',
                    weekdays=list(range(1, 8)) if weekly else [], item='整理书包' if weekly else '阅读' if clock else '喝水',
                    text='时间到啦，休息一下吧。', clarify='')
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
        provider = Provider()
        settings = {'device_token': 'a' * 64}
        service = VoiceService(settings, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            await exercise(settings, provider, f'ws://127.0.0.1:{port}/stubuddy/ai')
        history = service.histories['02:00:00:00:04:42']
        self.assertEqual(len(history.pairs), 8)
        self.assertIn('第一条', history.pairs[1][1])

    async def test_four_reboot_cache_restores_are_serial_not_cancelled_and_add_no_history(self):
        provider = mock.Mock()
        provider.synthesize = mock.AsyncMock(return_value=baseline.sine_pcm(16000))
        service = VoiceService({'device_token': 'a' * 64}, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f'ws://127.0.0.1:{port}/stubuddy/ai', additional_headers={
                    'Authorization': 'Bearer ' + 'a' * 64, 'Device-Id': '02:00:00:00:04:43', 'Protocol-Version': '1'}) as socket:
                await socket.send(json.dumps({'type': 'hello', 'transport': 'websocket', 'ai_protocol': 1,
                    'reply_stream': 1, 'max_reply_packet_bytes': 1275, 'device_controls': 1,
                    'volume_restore': 1, 'countdown': 1, 'scheduled': 1, 'audio_params': baseline.AUDIO_PARAMS}))
                await socket.recv()
                ids = [char * 16 for char in 'abcd']
                for identity in ids:
                    await socket.send(json.dumps({'type': 'countdown_sync', 'id': identity, 'text': '喝水时间到啦。'}))
                finished = []
                while len(finished) < 4:
                    message = json.loads(await asyncio.wait_for(socket.recv(), 5))
                    if message.get('state') == 'end': finished.append(message['id'])
                self.assertEqual(finished, ids)
                await socket.send('{"type":"ping"}')
                self.assertEqual(json.loads(await socket.recv())['type'], 'pong')
        self.assertEqual(len(service.histories['02:00:00:00:04:43'].pairs), 0)
