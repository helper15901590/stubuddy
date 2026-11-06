"""Regression for the actual DashScope ITN date/clock sample."""
from datetime import datetime
import unittest
from unittest import mock

from calendar_input import normalize, CalendarInputError
import reminder_tools as tools
import reminder_smoke
from test_reminders import provider_plan, status, row, NOW
from test_countdown import plan
from voice_server import VoiceService
import test_voice_ai as baseline


class CalendarInputTests(unittest.IsolatedAsyncioTestCase):
    def test_joined_date_dot_clock_requires_unique_split(self):
        for source in ('2026/11/0613.6分', '2026/11/0613.06分', '2026-11-0613．6分'):
            self.assertEqual(tools.clock_value(source)[0], 786)
            self.assertEqual(tools.date_value(source, NOW), '2026-11-06')
            self.assertEqual(normalize(normalize(source)), normalize(source))
        for source in ('2026/11/111.6分', '2026/02/3013.6分', '2026/11/0625.6分',
                       '2026/11/0613.60分', '2026/11/0613.6分钟后'):
            with self.assertRaises(CalendarInputError, msg=source): normalize(source)
        self.assertEqual(normalize('13.6分钟后提醒我看书'), '13.6分钟后提醒我看书')

    async def test_real_reported_joined_dot_clock_creates_exact_schedule(self):
        now = int(datetime(2026, 11, 6, 12, 0, tzinfo=tools.TZ).timestamp())
        writes = []
        async def execute(name, args):
            if name == tools.GET: return dict(status(), now=now)
            self.assertEqual(name, tools.START)
            writes.append(args)
            return dict(status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])]), now=now)
        reply = await tools.answer(provider_plan(item='看书', text='看书时间到啦。'),
            [{'content': '设置2026/11/0613.6分提醒我看书。'}], execute, mock.AsyncMock(), {})
        self.assertIn('十三点六分', reply)
        self.assertEqual(writes[0]['minute'], 786)
        self.assertEqual(writes[0]['deadline'], int(datetime(2026, 11, 6, 13, 6, tzinfo=tools.TZ).timestamp()))

    def test_period_dot_clock_is_not_decimal_duration(self):
        for source, expected in [('2026/11/06上午10.3分', 603), ('下午3.05分', 905),
                                 ('上午10．03分', 603)]:
            self.assertEqual(tools.clock_value(source)[0], expected)
        for source in ('10.3分钟后提醒我喝水', '上午10.3分钟后提醒我喝水'):
            self.assertEqual(normalize(source), source)
        with self.assertRaises(CalendarInputError): normalize('上午10.75分')

    async def test_dot_clock_with_existing_countdown_creates_scheduled_only(self):
        now = int(datetime(2026, 11, 6, 9, 0, tzinfo=tools.TZ).timestamp())
        timer = row(identity='b', kind='countdown')
        writes = []
        async def execute(name, args):
            if name == tools.GET: return dict(status([timer]), now=now)
            self.assertEqual(name, tools.START)
            writes.append(args)
            return dict(status([timer, dict(row(), id=args['cache_id'], item=args['item'],
                text=args['text'], deadline=args['deadline'], minute=args['minute'],
                weekdays=args['weekdays'])]), now=now)
        reply = await tools.answer(provider_plan(item='看书', text='看书时间到啦。'),
            [{'content': '设置2026/11/06上午10.3分提醒我看书。'}], execute, mock.AsyncMock(), {})
        self.assertIn('十点三分', reply)
        self.assertEqual(writes[0]['minute'], 603)
        self.assertEqual(writes[0]['deadline'], int(datetime(2026, 11, 6, 10, 3, tzinfo=tools.TZ).timestamp()))

    async def test_unparsed_calendar_minutes_never_fall_back_to_countdown(self):
        execute = mock.AsyncMock(return_value=status([row(kind='countdown')]))
        prepare = mock.AsyncMock()
        provider = provider_plan()
        reply = await tools.answer(provider, [{'content': '设置明天10分提醒我看书'}], execute, prepare, {})
        self.assertIn('几点', reply)
        self.assertNotIn('已经有一个倒计时', reply)
        self.assertEqual([call.args[0] for call in execute.call_args_list], [tools.GET])
        prepare.assert_not_called()
        provider.completion_message.assert_not_called()

    def test_actual_joined_date_and_ampm_variants(self):
        cases = {'2026/11/069:40a.m.': 580, '2026/11/069:40p.m.': 1300,
                 '2026/11/0609:40AM': 580, '2026-11-06 9:40 a. m.': 580,
                 '2026/11/06T09:40': 580, '2026/11/0619:40': 1180,
                 '2026/11/0612:00AM': 0, '2026/11/0612:00p.m.': 720}
        for source, expected in cases.items():
            normalized = normalize(source)
            self.assertEqual(normalize(normalized), normalized)
            self.assertEqual(tools.clock_value(source)[0], expected, source)
            self.assertEqual(tools.date_value(source, NOW), '2026-11-06', source)

    def test_ambiguous_and_invalid_forms_do_not_guess_a_day_or_hour(self):
        for text in ('2026/11/111:40pm', '2026/11/111:40', '2026/02/309:40AM',
                     '2026/11/0613:40PM', '2026/11/0619点40分', '2026/11/069:75AM',
                     '上午9:40PM', '2026/11/06 0:40AM'):
            with self.assertRaises(CalendarInputError, msg=text): normalize(text)
            self.assertIsNone(tools.clock_value(text)[0], text)

    def test_weekly_pm_and_existing_chinese_forms_remain_correct(self):
        for text, expected in [('每周一9:40p.m.提醒我看书', 1300), ('每周一晚上九点四十分提醒我看书', 1300),
                               ('每周日零点提醒我喝水', 0), ('09:40提醒我看书', 580)]:
            self.assertEqual(tools.clock_value(text)[0], expected)

    async def test_ambiguous_calendar_does_not_read_prepare_or_write(self):
        execute, prepare = mock.AsyncMock(), mock.AsyncMock()
        provider = provider_plan()
        source = '设置2026/11/111:40pm提醒我看书'
        self.assertTrue(tools.intent(source))
        reply = await tools.answer(provider, [{'content': source}], execute, prepare, {})
        self.assertIn('分开说', reply)
        execute.assert_not_called()
        prepare.assert_not_called()
        provider.completion_message.assert_not_called()

    async def test_literal_failure_sample_uses_exact_date_and_clock(self):
        now = int(datetime(2026, 11, 6, 9, 0, tzinfo=tools.TZ).timestamp())
        stored = []
        async def execute(name, args):
            if name == tools.GET: return dict(status(), now=now)
            stored.append(args)
            return dict(status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])]), now=now)
        provider = provider_plan(item='看书', text='看书时间到啦。', time='00:00')
        reply = await tools.answer(provider, [{'content': '设置2026/11/069:40a.m.提醒我看书。'}], execute, mock.AsyncMock(), {})
        self.assertIn('九点四十分', reply)
        self.assertEqual(stored[0]['minute'], 580)
        self.assertEqual(stored[0]['deadline'], int(datetime(2026, 11, 6, 9, 40, tzinfo=tools.TZ).timestamp()))

    async def test_real_router_opus_and_receipt_for_original_asr_text(self):
        now = int(datetime(2026, 11, 6, 9, 0, tzinfo=tools.TZ).timestamp())
        class FixedDevice(reminder_smoke.Device):
            def apply(self, name, args):
                reply = super().apply(name, args)
                reply['now'] = now
                return reply
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.query = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return self.query
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
            async def answer(self, messages): raise AssertionError('Calendar command escaped router')
            async def completion_message(self, body):
                return plan(kind='scheduled', seconds=0, date='', time='00:00', weekdays=[],
                            item='看书', text='看书时间到啦。', clarify='')
        queries = ['设置2026/11/069:40a.m.提醒我看书。', '定时提醒是什么内容？', '我有哪些提醒？']
        def verify(turn, answer, device, calls, caches):
            self.assertEqual(calls, [tools.GET, tools.START] if turn == 1 else [tools.GET])
            self.assertEqual(caches, int(turn == 1))
            self.assertEqual(device.records[0]['minute'], 580)
            self.assertEqual(len(device.records), 1)
        provider = Provider()
        service = VoiceService({'device_token': 'a' * 64}, provider=provider)
        with mock.patch.object(reminder_smoke, 'Device', FixedDevice):
            async with service.server(port=0) as server:
                port = server.sockets[0].getsockname()[1]
                await reminder_smoke.exercise({'device_token': 'a' * 64}, provider,
                    f'ws://127.0.0.1:{port}/stubuddy/ai', queries, verify)
