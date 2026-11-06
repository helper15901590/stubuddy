"""Raw ASR phrases cross the real router, receipts, Opus and eight-round history."""
import asyncio
import json
import time
import unittest
from unittest import mock

import httpx
import capabilities
import reminder_tools as tools
from providers import DashScope, ProviderFailure
from reminder_smoke import routing_exercise, exercise
from voice_server import VoiceService
from test_reminders import row, status, provider_plan, NOW
from test_countdown import plan
import test_voice_ai as baseline


class RoutingPolicyTests(unittest.IsolatedAsyncioTestCase):
    def test_help_uses_negotiated_capabilities_not_last_topic(self):
        for query in ('你有哪些功能？', '你有什么工具？', '你可以干什么？', '提醒功能怎么用？'):
            reply = capabilities.answer(query, True, True, True, True)
            self.assertIsNotNone(reply)
            self.assertIn('提醒', reply)
        reply = capabilities.answer('你有哪些功能？')
        self.assertNotIn('三个', reply)
        self.assertNotIn('调节', reply)
        self.assertIsNone(capabilities.answer('计算器有哪些功能？'))
        self.assertIsNone(capabilities.answer('帮我设置每周一晚上八点提醒我看书'))

    def test_clock_provenance_missing_ambiguous_zero_and_twenty_four_hour_forms(self):
        for query, minute in [('每周一17点提醒我', 1020), ('晚上八点', 1200), ('00:00', 0),
                ('凌晨十二点', 0), ('每周日零点', 0), ('中午十二点十分', 730),
                ('上午八点半', 510), ('晚上八点一刻', 1215), ('下午一点三刻', 825)]:
            self.assertEqual(tools.clock_value(query)[0], minute, query)
        for query in ('每周一三五提醒我看书', '八点', '25:00', '20:70', '晚上八点和九点'):
            self.assertIsNone(tools.clock_value(query)[0], query)

    async def test_log_count_phrases_are_read_only_even_with_full_slots_and_old_refused_creation(self):
        records = [row(), row('b', minutes=30), row('c', minutes=50), row('d', 'countdown')]
        provider = provider_plan(kind='countdown', seconds=120, item='休息')  # Deliberately dangerous model default.
        for query in ('我现在有几条提醒。', '我现在有多少条提醒？', '现在还有几条提醒。',
                      '现在有多少倒计时？', '现在还有几条。'):
            execute = mock.AsyncMock(return_value=status(records))
            prepare = mock.AsyncMock()
            view = {'topic': 'reminders'}
            messages = [{'role': 'user', 'content': '设置两分钟后提醒我休息'},
                        {'role': 'assistant', 'content': '已有倒计时，请先取消'}, {'role': 'user', 'content': query}]
            reply = await tools.answer(provider, messages, execute, prepare, view)
            self.assertIn('四条', reply)
            self.assertEqual(execute.await_args_list, [mock.call(tools.GET, {})])
            prepare.assert_not_called()
        provider.completion_message.assert_not_called()

    async def test_missing_time_never_prepares_or_saves_default_midnight(self):
        provider, execute, prepare = provider_plan(time='00:00'), mock.AsyncMock(return_value=status()), mock.AsyncMock()
        view = {}
        reply = await tools.answer(provider, [{'content': '每周一、周三、周五，提醒我看书。'}], execute, prepare, view)
        self.assertIn('几点', reply)
        self.assertEqual(view['draft']['weekdays'], 21)
        self.assertIsNone(view['draft']['minute'])
        prepare.assert_not_called()
        provider.completion_message.assert_not_called()

    async def test_ambiguous_clock_with_minutes_is_not_misread_as_relative_duration(self):
        for query in ('明天八点十分提醒我喝水', '每周一八点三十分提醒我看书'):
            provider = provider_plan(kind='countdown', seconds=120)
            prepare, execute = mock.AsyncMock(), mock.AsyncMock(return_value=status())
            answer = await tools.answer(provider, [{'content': query}], execute, prepare, {})
            self.assertIn('早上', answer)
            prepare.assert_not_called()
            self.assertEqual(execute.await_count, 1)

    async def test_draft_completion_ignores_model_time_and_completed_history(self):
        provider = provider_plan(time='00:00', weekdays=[7], item='看书', text='看书时间到啦。')
        view, writes = {}, []
        async def execute(name, args):
            if name == tools.GET: return status()
            writes.append(args)
            return status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])])
        await tools.answer(provider, [{'content': '每周一、三、五提醒我看书'}], execute, mock.AsyncMock(), view)
        old_history = [{'role': 'user', 'content': '设置两分钟后提醒我休息'},
                       {'role': 'assistant', 'content': '旧任务已拒绝'}, {'role': 'user', 'content': '晚上八点'}]
        await tools.answer(provider, old_history, execute, mock.AsyncMock(), view)
        self.assertEqual(writes[0]['minute'], 1200)
        self.assertEqual(writes[0]['weekdays'], 21)
        body = provider.completion_message.await_args.args[0]
        self.assertNotIn('两分钟后', json.dumps(body, ensure_ascii=False))
        self.assertNotIn('draft', view)

    async def test_weekdays_fragment_completes_only_live_pending_task(self):
        for fragment in ('周一、周三、周五', '一、三、五', '每周一到周五'):
            view = {}
            execute = mock.AsyncMock(return_value=status())
            provider = provider_plan(item='喝水')
            await tools.answer(provider, [{'content': '每周上午八点提醒我喝水'}], execute, mock.AsyncMock(), view)
            self.assertIn('draft', view)
            self.assertEqual(tools.action(fragment, view=view), 'complete')
            self.assertEqual(tools.action(fragment, view={}), 'clarify' if '周' in fragment else None)

    async def test_expired_or_cancelled_draft_cannot_create(self):
        view = {}
        execute, prepare = mock.AsyncMock(return_value=status()), mock.AsyncMock()
        await tools.answer(mock.Mock(), [{'content': '每周一提醒我看书'}], execute, prepare, view)
        view['draft']['expires'] = time.monotonic() - 1
        self.assertEqual(tools.action('晚上八点', view=view), 'orphan')
        self.assertNotIn('draft', view)
        await tools.answer(mock.Mock(), [{'content': '每周一提醒我看书'}], execute, prepare, view)
        reply = await tools.answer(mock.Mock(), [{'content': '算了'}], execute, prepare, view)
        self.assertIn('已取消', reply)
        self.assertNotIn('draft', view)
        prepare.assert_not_called()
        await tools.answer(mock.Mock(), [{'content': '每周一提醒我看书'}], execute, prepare, view)
        await tools.answer(mock.Mock(), [{'content': '取消提醒'}], execute, prepare, view)
        self.assertNotIn('draft', view)

    async def test_unknown_reminder_utterance_cannot_fall_through_to_creation(self):
        execute, prepare = mock.AsyncMock(return_value=status()), mock.AsyncMock()
        reply = await tools.answer(provider_plan(), [{'content': '提醒'}], execute, prepare, {})
        self.assertIn('查询', reply)
        prepare.assert_not_called()
        self.assertEqual(execute.await_count, 1)

    async def test_number_only_selection_requires_a_pending_cancellation(self):
        execute = mock.AsyncMock(side_effect=[status([row()]), status([row()]), status()])
        view = {}
        await tools.answer(mock.Mock(), [{'content': '删除全部提醒'}], execute, mock.Mock(), view)
        self.assertEqual(tools.action('第一条', view=view), 'cancel')
        self.assertIsNone(tools.action('第一条', view={}))
        reply = await tools.answer(mock.Mock(), [{'content': '第一条'}], execute, mock.Mock(), view)
        self.assertIn('已取消', reply)
        self.assertEqual(execute.await_args.args, (tools.CANCEL, {'cache_id': 'a' * 16}))
        self.assertNotIn('cancel_until', view)

    async def test_period_only_reply_completes_ambiguous_clock(self):
        view, writes = {}, []
        provider = provider_plan(item='看书', text='看书时间到啦。')
        async def execute(name, args):
            if name == tools.GET: return status()
            writes.append(args)
            return status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])])
        await tools.answer(provider, [{'content': '每周一八点十分提醒我看书'}], execute, mock.AsyncMock(), view)
        await tools.answer(provider, [{'content': '晚上'}], execute, mock.AsyncMock(), view)
        self.assertEqual(writes[0]['minute'], 1210)

    async def test_missing_subject_can_be_completed_without_losing_verified_clock(self):
        view, writes = {}, []
        provider = provider_plan(clarify='做什么？')
        async def execute(name, args):
            if name == tools.GET: return status()
            writes.append(args)
            return status([dict(row(), id=args['cache_id'], item=args['item'], text=args['text'],
                deadline=args['deadline'], minute=args['minute'], weekdays=args['weekdays'])])
        await tools.answer(provider, [{'content': '明天20:00提醒我'}], execute, mock.AsyncMock(), view)
        self.assertFalse(writes)
        self.assertEqual(tools.action('一加一等于几', view=view), None)
        provider = provider_plan(item='看书', text='看书时间到啦。')
        await tools.answer(provider, [{'content': '看书'}], execute, mock.AsyncMock(), view)
        self.assertEqual(writes[0]['minute'], 1200)
        self.assertEqual(writes[0]['item'], '看书')

    async def test_complete_original_log_over_websocket(self):
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.query = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return self.query
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
            async def completion_message(self, body):
                query = body['messages'][-1]['content']
                subject = '看书' if '看书' in query else '整理书包' if '整理书包' in query else '喝水'
                # Wrong defaults must not become authorizations/time evidence.
                return plan(kind='countdown', seconds=120, date='', time='00:00', weekdays=[7],
                    item=subject, text=subject + '时间到啦。', clarify='')
            async def answer(self, messages):
                raise AssertionError('A device operation escaped to ordinary chat')
        provider = Provider()
        settings = {'device_token': 'a' * 64}
        service = VoiceService(settings, provider=provider)
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            device, answers, calls = await routing_exercise(settings, provider, f'ws://127.0.0.1:{port}/stubuddy/ai')
        self.assertFalse(device.records)
        self.assertEqual(len(service.histories['02:00:00:00:04:42'].pairs), 8)
        self.assertEqual(answers[-5], '现在没有提醒。')

    async def test_unrelated_chat_clears_draft_and_help_does_not_choose_volume_target(self):
        class Provider:
            settings = {}
            async def synthesize(self, text):
                self.query = text
                return baseline.sine_pcm(16000)
            async def recognize(self, pcm): return self.query
            async def synthesize_stream(self, text): yield baseline.sine_pcm(16000)
            async def answer(self, messages):
                return '二。' if '一加一' in messages[-1]['content'] else '请完整说明提醒事项和时间。'
            async def completion_message(self, body):
                if list(body['tools'][0]['function']['parameters']['properties']) == ['volume']:
                    return plan(volume=60)
                raise AssertionError('Unexpected creation plan')
        expected = [[tools.GET], [], [], [], [], [], [tools.GET, tools.VOLUME], [tools.GET]]
        queries = ['每周一提醒我看书', '一加一等于几', '晚上八点', '你有什么工具？',
                   '调到60', '调到60', '提醒音量调到60', '我有哪些提醒？']
        def verify(turn, answer, device, calls, caches):
            self.assertEqual(calls, expected[turn - 1])
            self.assertFalse(device.records)
            self.assertEqual(caches, 0)
            if turn in (5, 6): self.assertIn('哪', answer)
        service = VoiceService({'device_token': 'a' * 64}, provider=Provider())
        async with service.server(port=0) as server:
            port = server.sockets[0].getsockname()[1]
            device, _, _ = await exercise({'device_token': 'a' * 64}, service.provider,
                f'ws://127.0.0.1:{port}/stubuddy/ai', queries, verify)
        self.assertEqual(device.volume, 60)


class ModelRetryTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, handle):
        provider = DashScope({'api_key': 'fixture-not-a-real-key',
            'base_url': 'https://dashscope.aliyuncs.com/compatible-mode/v1',
            'tts_ws_url': 'wss://dashscope.aliyuncs.com/api-ws/v1/inference'})
        provider.client = httpx.AsyncClient(transport=httpx.MockTransport(handle))
        self.addAsyncCleanup(provider.aclose)
        return provider

    async def test_500_then_success_retries_once_without_logging_body(self):
        attempts = []
        def handle(request):
            attempts.append(request)
            return (httpx.Response(500, json={'error': 'fixture-secret-body'}) if len(attempts) == 1 else
                    httpx.Response(200, json={'choices': [{'message': {'content': '成功'}}]}))
        provider = self.provider(handle)
        with self.assertLogs('stubuddy.ai', level='WARNING') as logs:
            self.assertEqual(await provider.answer([{'role': 'user', 'content': '你好'}]), '成功')
        self.assertEqual(len(attempts), 2)
        self.assertNotIn('fixture-secret', ''.join(logs.output))

    async def test_second_500_fails_and_401_429_are_not_retried(self):
        for code, expected in ((500, 2), (401, 1), (429, 1)):
            attempts = []
            def handle(request):
                attempts.append(request)
                return httpx.Response(code)
            provider = self.provider(handle)
            with self.assertRaises(ProviderFailure) as error:
                await provider.answer([{'role': 'user', 'content': '你好'}])
            self.assertEqual(error.exception.code, 'http_' + str(code))
            self.assertEqual(len(attempts), expected)

    async def test_cancel_during_backoff_does_not_retry_or_delay_cancellation(self):
        entered, calls = asyncio.Event(), []
        def handle(request):
            calls.append(request)
            entered.set()
            return httpx.Response(500)
        provider = self.provider(handle)
        task = asyncio.create_task(provider.answer([{'role': 'user', 'content': '旧回合'}]))
        await asyncio.wait_for(entered.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError): await task
        self.assertEqual(len(calls), 1)

    async def test_retry_shares_original_total_budget(self):
        calls = []
        def handle(request):
            calls.append(request)
            return httpx.Response(500)
        provider = self.provider(handle)
        with self.assertRaises(TimeoutError):
            await provider.completion_message({'messages': []}, timeout=0.03)
        self.assertEqual(len(calls), 1)
