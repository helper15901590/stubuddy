"""Calendar intent cannot silently become a countdown; incomplete tasks clarify."""
from datetime import datetime
import unittest
from unittest import mock

import clock_answers as clock
import reminder_tools as tools
from test_reminders import status, row, provider_plan


class CalendarGuardTests(unittest.IsolatedAsyncioTestCase):
    async def test_generic_countdown_fixed_voice_and_ack(self):
        for queries in (['倒计时一分钟'], ['请帮我设置一个倒计时', '一分钟之后'], ['十分钟后提醒我']):
            view, writes = {}, []
            provider, prepare = provider_plan(), mock.AsyncMock()
            async def execute(name, args):
                if name == tools.GET: return status()
                self.assertEqual(name, tools.TIMER)
                writes.append(args)
                return status([dict(row(kind='countdown'), id=args['cache_id'], item=args['item'],
                    text=args['text'], deadline=status()['now'] + args['seconds'])])
            for query in queries:
                reply = await tools.answer(provider, [{'content': query}], execute, prepare, view)
            self.assertEqual(len(writes), 1)
            self.assertEqual(writes[0]['item'], '倒计时')
            self.assertIn('已设置', reply)
            prepare.assert_awaited_once_with('时间到啦。', writes[0]['cache_id'])
            provider.completion_message.assert_not_called()

    async def test_default_timer_limits_and_failed_receipt(self):
        for query, records in [('倒计时30秒', []), ('倒计时25小时', []), ('倒计时一分钟', [row(kind='countdown')])]:
            prepare, execute = mock.AsyncMock(), mock.AsyncMock(return_value=status(records))
            reply = await tools.answer(provider_plan(), [{'content': query}], execute, prepare, {})
            self.assertNotIn('已设置', reply)
            prepare.assert_not_called()
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
        execute = mock.AsyncMock(side_effect=[status(), dict(status(ok=False), error=3)])
        reply = await tools.answer(provider_plan(), [{'content': '倒计时一分钟'}], execute, mock.AsyncMock(), {})
        self.assertNotIn('已设置', reply)

    def test_default_timer_keeps_activity_and_ambiguity(self):
        for source in ('倒计时一分钟提醒我喝水', '设置一个倒计时\n补充：吃饭\n补充：一分钟', '倒计时一分钟那个'):
            self.assertFalse(tools.timer_without_subject(source))

    async def test_countdown_duration_fragments_cannot_escape_to_chat(self):
        view, execute, prepare = {}, mock.AsyncMock(return_value=status()), mock.AsyncMock()
        provider = provider_plan()
        for query, expected in [('设置一个倒计时。', '多久'), ('30s钟之后。', '一分钟到二十四小时'),
                                ('吃饭。', '多久')]:
            self.assertTrue(tools.intent(query, view=view), query)
            reply = await tools.answer(provider, [{'content': query}], execute, prepare, view)
            self.assertIn(expected, reply)
        prepare.assert_not_called()
        provider.completion_message.assert_not_called()
        self.assertTrue(all(call.args[0] == tools.GET for call in execute.call_args_list))

    async def test_vague_subject_and_malformed_plan_clarify_without_write(self):
        for response in (None, {}, {'tool_calls': [{'function': {'name': 'countdown_plan', 'arguments': '{'}}]}):
            provider, view = provider_plan(), {}
            execute, prepare = mock.AsyncMock(return_value=status()), mock.AsyncMock()
            await tools.answer(provider, [{'content': '明天晚上七点提醒我。'}], execute, prepare, view)
            reply = await tools.answer(provider, [{'content': '那是。'}], execute, prepare, view)
            self.assertIn('没有听清', reply)
            provider.completion_message.assert_not_called()
            if response is not None:
                provider.completion_message.return_value = response
                reply = await tools.answer(provider, [{'content': '吃饭'}], execute, prepare, view)
                self.assertIn('再说一次', reply)
            prepare.assert_not_called()
            self.assertTrue(all(call.args[0] == tools.GET for call in execute.call_args_list))

    def test_clock_phrases_are_bounded_and_numbers_are_spoken(self):
        for query in ('现在时间是多少？', '当前时间是几点？', '现在几点几分？', '请问现在的时间是什么呢？',
                      '给我报一下时间。', '报时。', '现在时间是什么？'):
            self.assertEqual(clock.intent(query)[0], 'time', query)
        for query in ('每天几点提醒我喝水', '现在几点提醒我看书', '给我报一下时间复杂度', '北京时间和伦敦差多少'):
            self.assertIsNone(clock.intent(query), query)
        for hour, minute, expected in ((0, 0, '零点整'), (10, 7, '十点七分'), (12, 50, '十二点五十分'),
                                       (23, 59, '二十三点五十九分')):
            answer = clock.answer(clock.intent('现在几点'), datetime(2026, 11, 6, hour, minute, tzinfo=clock.TZ).timestamp())
            self.assertIn(expected, answer)
            self.assertFalse(any(char.isdigit() for char in answer))

    async def test_repeat_and_weekday_fragments_never_create_countdown(self):
        for query in ('每天10.07分提醒我吃饭', '每周一10.07分提醒我吃饭', '星期一10分提醒我吃饭',
                      '每天十分钟后提醒我吃饭', '每周一设置十分钟倒计时提醒我吃饭'):
            provider, prepare = provider_plan(), mock.AsyncMock()
            execute = mock.AsyncMock(return_value=status([row(kind='countdown')]))
            reply = await tools.answer(provider, [{'content': query}], execute, prepare, {})
            self.assertIn('几点', reply, query)
            self.assertNotIn('已经有一个倒计时', reply)
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
            prepare.assert_not_called()
            provider.completion_message.assert_not_called()

    async def test_repeat_continuation_discards_previous_duration(self):
        execute = mock.AsyncMock(return_value=status())
        view = {}
        await tools.answer(provider_plan(), [{'content': '十分钟后提醒我吃饭'}], execute, mock.AsyncMock(), view)
        reply = await tools.answer(provider_plan(), [{'content': '每天提醒我'}], execute, mock.AsyncMock(), view)
        self.assertIn('几点', reply)
        self.assertIsNone(view['draft']['seconds'])

    async def test_reported_two_turns_then_subject_preserve_tomorrow_clock(self):
        now = int(datetime(2026, 11, 6, 10, 0, tzinfo=tools.TZ).timestamp())
        writes = []
        async def execute(name, args):
            if name == tools.GET: return dict(status(), now=now)
            self.assertEqual(name, tools.START)
            writes.append(args)
            return dict(status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])]), now=now)
        provider, prepare, view = provider_plan(item='喝水', text='喝水时间到啦。'), mock.AsyncMock(), {}
        first = await tools.answer(provider, [{'content': '明天提醒我。'}], execute, prepare, view)
        self.assertIn('几点', first)
        second = await tools.answer(provider, [{'content': '明天晚上七点提醒我。'}], execute, prepare, view)
        self.assertIn('做什么', second)
        provider.completion_message.assert_not_called()
        prepare.assert_not_called()
        third = await tools.answer(provider, [{'content': '喝水'}], execute, prepare, view)
        self.assertIn('喝水', third)
        self.assertEqual(writes[0]['deadline'], int(datetime(2026, 11, 7, 19, 0, tzinfo=tools.TZ).timestamp()))

    async def test_empty_model_subject_clarifies_instead_of_failing(self):
        execute = mock.AsyncMock(return_value=status())
        prepare = mock.AsyncMock()
        reply = await tools.answer(provider_plan(item='', text='', clarify=''),
            [{'content': '明天晚上七点设置提醒'}], execute, prepare, {})
        self.assertIn('做什么', reply)
        prepare.assert_not_called()
        self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])

    async def test_unclear_weekday_replacement_does_not_reuse_old_clock(self):
        view, execute, prepare = {}, mock.AsyncMock(return_value=status()), mock.AsyncMock()
        provider = provider_plan()
        reply = await tools.answer(provider, [{'content': '每周上午八点提醒我喝水'}], execute, prepare, view)
        self.assertIn('哪几天', reply)
        reply = await tools.answer(provider, [{'content': '每周一10.07分提醒我喝水'}], execute, prepare, view)
        self.assertIn('几点', reply)
        self.assertIsNone(view['draft']['minute'])
        prepare.assert_not_called()
        provider.completion_message.assert_not_called()
