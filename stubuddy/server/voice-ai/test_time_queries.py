"""Clock, subject phrases and ASR clarification with real local ai/1/Opus."""
import time
from datetime import datetime
import unittest
from unittest import mock

import clock_answers as clock
import reminder_tools as tools
import reminder_smoke
from voice_server import VoiceService
from test_reminders import row, status
import test_voice_ai as baseline


class TimeQueryTests(unittest.IsolatedAsyncioTestCase):
    def test_asr_day_shift_is_not_hidden_by_correct_calendar_answer(self):
        stamp = int(datetime(2026, 11, 6, 9, 18, tzinfo=clock.TZ).timestamp())
        heard = '前天几号？'
        correct_for_heard = clock.answer(clock.intent(heard), stamp)
        with self.assertRaises(reminder_smoke.ClockCheckFailure) as error:
            reminder_smoke.verify_clock('今天几号？', heard, correct_for_heard, stamp)
        self.assertEqual(error.exception.code, 'asr_clock_intent_mismatch')
        reminder_smoke.verify_clock('请告诉我今天的日期是什么？', '今天几号？',
                                   clock.answer(clock.intent('今天几号？'), stamp), stamp)
        with self.assertRaises(reminder_smoke.ClockCheckFailure) as error:
            reminder_smoke.verify_clock('今天几号？', '今天几号？', correct_for_heard, stamp)
        self.assertEqual(error.exception.code, 'clock_answer_mismatch')
        with self.assertRaises(reminder_smoke.ClockCheckFailure) as error:
            reminder_smoke.verify_clock('今天几号？', None, '', None)
        self.assertEqual(error.exception.code, 'asr_clock_intent_mismatch')

    def test_beijing_midnight_cross_day_weekday_and_no_model_defaults(self):
        timestamp = datetime(2026, 11, 6, 23, 59, tzinfo=clock.TZ).timestamp()
        self.assertEqual(clock.answer(clock.intent('现在几点？'), timestamp), '现在是北京时间二十三点五十九分。')
        self.assertEqual(clock.answer(clock.intent('今天几号？'), timestamp), '今天是二零二六年十一月六日，星期五。')
        self.assertEqual(clock.answer(clock.intent('明天星期几？'), timestamp), '明天是星期六。')
        self.assertEqual(clock.answer(clock.intent('今天几号？'), timestamp + 60), '今天是二零二六年十一月七日，星期六。')
        self.assertEqual(clock.answer(clock.intent('后天几号？'), timestamp), '后天是二零二六年十一月八日，星期日。')
        for query in ('请问一下现在几点了？', '现在北京时间几点？', '现在的时间是什么？', '几点了？'):
            self.assertEqual(clock.intent(query)[0], 'time')
        for query in ('今天的日期是什么？', '现在什么日期？'):
            self.assertEqual(clock.intent(query)[0], 'date')
        for query in ('现在几点提醒我喝水', '每周一晚上八点提醒我', '一年有多少天', '一加一等于几'):
            self.assertIsNone(clock.intent(query), query)
        with self.assertRaises(ValueError): clock.answer(clock.intent('现在几点'), 0)

    async def test_subject_phrases_read_real_device_without_writes(self):
        provider, prepare = mock.Mock(), mock.AsyncMock()
        for query in ('定时提醒是什么内容？', '提醒事项是什么？', '提醒我做什么？', '提醒什么内容？'):
            execute = mock.AsyncMock(return_value=status([row(item='看书')]))
            self.assertTrue(tools.intent(query))
            answer = await tools.answer(provider, [{'content': query}], execute, prepare, {})
            self.assertEqual(answer, '提醒你看书。')
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
        prepare.assert_not_called()

    async def test_specific_schedule_subject_does_not_pick_countdown(self):
        execute = mock.AsyncMock(return_value=status([row('a', 'countdown', item='休息'), row('b', item='看书')]))
        answer = await tools.answer(mock.Mock(), [{'content': '定时提醒是什么内容？'}], execute, mock.Mock(), {})
        self.assertEqual(answer, '提醒你看书。')
        execute.return_value = status([row('a', 'countdown')])
        self.assertEqual(await tools.answer(mock.Mock(), [{'content': '定时提醒是什么内容？'}], execute, mock.Mock(), {}), '现在没有定时提醒。')

    async def test_typo_needs_reminder_context_then_confirmation_before_read(self):
        query = '我现在有多少提成？'
        self.assertIsNone(tools.action(query, view={}))
        view, execute, prepare = {'topic': 'reminders'}, mock.AsyncMock(return_value=status([row()])), mock.AsyncMock()
        reply = await tools.answer(mock.Mock(), [{'content': query}], execute, prepare, view)
        self.assertIn('提醒吗', reply)
        execute.assert_not_called()
        # Repeated route inspection cannot consume the pending confirmation.
        self.assertTrue(tools.intent('是的', view=view))
        self.assertTrue(tools.intent('是的', view=view))
        reply = await tools.answer(mock.Mock(), [{'content': '是的'}], execute, prepare, view)
        self.assertIn('一条', reply)
        self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
        prepare.assert_not_called()
        self.assertNotIn('query_clarification', view)

    async def test_decline_expiry_new_question_and_real_commission_are_not_rewritten(self):
        execute = mock.AsyncMock()
        for expired in (False, True):
            view = {'topic': 'reminders', 'query_clarification': {'expires': time.monotonic() + (-1 if expired else 60)}}
            if expired:
                self.assertIsNone(tools.action('是的', view=view))
            else:
                answer = await tools.answer(mock.Mock(), [{'content': '不是'}], execute, mock.Mock(), view)
                self.assertIn('重新说', answer)
            self.assertNotIn('query_clarification', view)
        self.assertIsNone(tools.action('提成怎么算？', view={'topic': 'reminders'}))
        view = {'topic': 'reminders', 'query_clarification': {'expires': time.monotonic() + 60}}
        self.assertEqual(tools.action('我有哪些提醒', view=view), 'list')
        self.assertNotIn('query_clarification', view)
        execute.assert_not_called()

    async def test_actual_router_reads_device_clock_and_cancels_clarification_on_topic_change(self):
        stamp = int(datetime(2026, 11, 6, 23, 59, tzinfo=clock.TZ).timestamp())
        class FixedDevice(reminder_smoke.Device):
            def apply(self, name, args):
                result = super().apply(name, args)
                result['now'] = stamp
                return result
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.query = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return self.query
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
            async def answer(self, messages):
                return '请再说一下你的问题。'
            async def completion_message(self, body): raise AssertionError('Read-only query called planner')
        queries = ['现在几点？', '今天几号？', '今天星期几？', '明天几号？', '我有哪些提醒？',
                   '我现在有多少提成？', '是的', '我现在有多少提成？', '不是',
                   '定时提醒是什么内容？', '我现在有多少提成？', '一加一等于几？', '是的']
        expected = [[tools.GET]] * 5 + [[], [tools.GET], [], [], [tools.GET], [], [], []]
        def verify(turn, answer, device, calls, caches):
            self.assertEqual(calls, expected[turn - 1])
            self.assertEqual(caches, 0)
            self.assertFalse(device.records)
            if turn <= 4: self.assertEqual(answer, clock.answer(clock.intent(queries[turn - 1]), stamp))
        provider = Provider()
        service = VoiceService({'device_token': 'a' * 64}, provider=provider)
        with mock.patch.object(reminder_smoke, 'Device', FixedDevice):
            async with service.server(port=0) as server:
                port = server.sockets[0].getsockname()[1]
                await reminder_smoke.exercise({'device_token': 'a' * 64}, provider,
                    f'ws://127.0.0.1:{port}/stubuddy/ai', queries, verify)
        self.assertEqual(len(service.histories['02:00:00:00:04:42'].pairs), 8)

    async def test_packaged_sixteen_round_smoke_over_actual_server_and_opus(self):
        from test_countdown import plan
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.query = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return self.query
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
            async def answer(self, messages): raise AssertionError('Device query escaped to LLM')
            async def completion_message(self, body):
                source = body['messages'][-1]['content']
                subject = '看书' if '看书' in source else '吃饭' if '吃饭' in source else '休息'
                return plan(kind='countdown', seconds=60, date='', time='00:00', weekdays=[],
                    item=subject, text=subject + '时间到啦。', clarify='')
        provider = Provider()
        service = VoiceService({'device_token': 'a' * 64}, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            device, _, _ = await reminder_smoke.time_query_exercise({'device_token': 'a' * 64}, provider,
                f'ws://127.0.0.1:{port}/stubuddy/ai')
        self.assertEqual(len(device.records), 1)

    def test_smoke_clocks_are_unambiguous_in_every_hour(self):
        for hour in range(24):
            when = datetime(2026, 11, 6, hour, 5, tzinfo=clock.TZ)
            self.assertEqual(tools.clock_value(reminder_smoke.explicit_clock(when))[0], hour * 60 + 5)
