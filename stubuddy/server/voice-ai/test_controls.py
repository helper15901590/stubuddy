"""Local-only device tool contracts, cancellation and acknowledgement tests."""
import asyncio
import contextlib
import json
import unittest
from unittest import mock

from device_tools import GET, SET, RESTORE, MODEL_GET, MODEL_SET, MODEL_RESTORE, arguments, result_volume, answer_with_tools, volume_intent, volume_speech_text, chinese_volume
from providers import DashScope
import test_voice_ai as baseline
NATIVE_OPUS = baseline.NATIVE_OPUS


def tool(name, values, call_id="model-call"):
    return {"role": "assistant", "content": None, "tool_calls": [
        {"id": call_id, "type": "function", "function": {"name": {GET: MODEL_GET, SET: MODEL_SET, RESTORE: MODEL_RESTORE}[name], "arguments": json.dumps(values)}}]}


class Planner:
    settings = {}
    answer_text = DashScope.answer_text
    content_text = staticmethod(DashScope.content_text)
    def __init__(self, responses):
        self.responses = iter(responses)
        self.requests = []
    async def completion_message(self, body):
        self.requests.append(body)
        return next(self.responses)


class ToolPolicyTests(unittest.IsolatedAsyncioTestCase):
    async def test_short_numeric_setting_uses_latest_volume_context_only(self):
        for query, target in [('调到100。',100), ('设成60',60), ('请调整到百分之六十',60)]:
            planner = Planner([tool(GET,{}), tool(SET,{'volume':target})])
            execute = mock.AsyncMock(side_effect=[{'ok':True,'volume':30}, {'ok':True,'volume':target}])
            messages = [{'role':'user','content':'现在音量多少'}, {'role':'assistant','content':'当前对话音量是百分之30。'},
                        {'role':'user','content':query}]
            self.assertTrue(volume_intent(query, messages))
            self.assertEqual(await answer_with_tools(planner,messages,execute,True), f'已将对话音量调整到百分之{target}。')
            self.assertEqual(execute.await_args_list,[mock.call(GET,{}),mock.call(SET,{'volume':target})])
            self.assertEqual(messages[-1]['content'],query)
        execute = mock.AsyncMock()
        provider = mock.Mock()
        for previous in [[], [{'role':'user','content':'1+1'}, {'role':'assistant','content':'2'}]]:
            messages = previous + [{'role':'user','content':'调到100'}]
            self.assertFalse(volume_intent('调到100', messages))
            self.assertIn('你是想调节音量吗', await answer_with_tools(provider,messages,execute,True))
        execute.assert_not_called()
        provider.answer.assert_not_called()

    def test_volume_spoken_numbers_are_bounded_and_general_math_is_unchanged(self):
        for number, word in [(0,'零'),(9,'九'),(10,'十'),(11,'十一'),(20,'二十'),(50,'五十'),(60,'六十'),(99,'九十九'),(100,'一百')]:
            self.assertEqual(chinese_volume(number), word)
            for prefix in ['当前对话音量是', '已将对话音量调整到', '已将对话音量恢复到']:
                self.assertEqual(volume_speech_text(prefix + f'百分之{number}。'), prefix + f'百分之{word}。')
        for text in ['1+1=2', '60除以10等于6。', '百分之60的学生', '已将对话音量调整到百分之101。']:
            self.assertEqual(volume_speech_text(text), text)
        for number in [-1,101,True]:
            with self.assertRaises(ValueError): chinese_volume(number)

    async def test_raw_tts_comparison_exports_wav_without_opus_or_device_gain(self):
        import tempfile
        import wave
        from pathlib import Path
        import smoke_test
        provider = mock.Mock()
        provider.synthesize = mock.AsyncMock(return_value=bytes(9600))
        provider.aclose = mock.AsyncMock()
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            with mock.patch.object(smoke_test, 'DashScope', return_value=provider):
                await smoke_test.check({},compare_dir=folder)
            self.assertEqual(len(list(folder.glob('*.wav'))),4)
            for file in folder.glob('*.wav'):
                with wave.open(str(file),'rb') as stream:
                    self.assertEqual((stream.getframerate(),stream.getnchannels(),stream.getsampwidth(),stream.getnframes()),(16000,1,2,4800))
        self.assertEqual([call.args[0] for call in provider.synthesize.await_args_list],
            ['已将对话音量调整到百分之60。','已将对话音量调整到百分之六十。',
             '已将对话音量调整到百分之50。','已将对话音量调整到百分之五十。'])
        provider.aclose.assert_awaited_once()

    async def test_short_restore_needs_recent_volume_context_and_real_device_ack(self):
        for query in ['恢复', '恢复。', '恢复一下吧']:
            planner = Planner([tool(GET, {})])
            execute = mock.AsyncMock(side_effect=[{'ok':True,'volume':0}, {'ok':True,'volume':40}])
            messages = [{'role':'user','content':'静音'}, {'role':'assistant','content':'已将音量调整到0'},
                        {'role':'user','content':query}]
            self.assertEqual(await answer_with_tools(planner,messages,execute,True), '已将对话音量恢复到百分之40。')
            self.assertEqual(execute.await_args_list, [mock.call(GET,{}),mock.call(RESTORE,{})])
            self.assertEqual(messages[-1]['content'], query)
        provider = mock.Mock()
        execute = mock.AsyncMock()
        for messages in [[{'role':'user','content':'恢复'}],
                         [{'role':'user','content':'1+1'}, {'role':'assistant','content':'2'}, {'role':'user','content':'恢复'}]]:
            self.assertEqual(await answer_with_tools(provider,messages,execute,True), '你是想恢复音量吗？请说恢复音量。')
        execute.assert_not_called()
        provider.answer.assert_not_called()

    async def test_plain_greeting_is_short_but_greeting_with_question_still_uses_model(self):
        provider = mock.Mock()
        provider.answer = mock.AsyncMock(return_value='2')
        execute = mock.AsyncMock()
        answer = await answer_with_tools(provider,[{'role':'user','content':'你好。'}],execute)
        self.assertLess(len(answer), 25)
        provider.answer.assert_not_called()
        self.assertEqual(await answer_with_tools(provider,[{'role':'user','content':'你好，1+1是多少'}],execute), '2')
        provider.answer.assert_awaited_once()
        execute.assert_not_called()

    async def test_arithmetic_followup_preserves_history_and_never_exposes_tools(self):
        provider = mock.Mock()
        provider.answer = mock.AsyncMock(return_value='3')
        provider.completion_message = mock.AsyncMock()
        execute = mock.AsyncMock()
        messages = [{'role':'system','content':'system'}, {'role':'user','content':'音量调到90'},
                    {'role':'assistant','content':'已将音量调到90'}, {'role':'user','content':'1+1'},
                    {'role':'assistant','content':'2'}, {'role':'user','content':'再加1'}]
        self.assertEqual(await answer_with_tools(provider, messages, execute), '3')
        provider.completion_message.assert_not_called()
        execute.assert_not_called()
        sent = provider.answer.await_args.args[0]
        self.assertEqual(sent[1:], messages[1:])
        self.assertEqual(messages[0]['content'], 'system')

    async def test_restore_uses_device_saved_state_not_a_model_guessed_number(self):
        for query in ['恢复音量', '恢复之前的音量', '取消静音', '解除静音', '音量恢复一下']:
            planner = Planner([tool(GET,{})])
            execute = mock.AsyncMock(side_effect=[{'ok': True, 'volume':0}, {'ok':True, 'volume':30}])
            answer = await answer_with_tools(planner,[{'role':'user','content':query}],execute,True)
            self.assertEqual(execute.await_args_list, [mock.call(GET,{}),mock.call(RESTORE,{})])
            self.assertEqual(answer,'已将对话音量恢复到百分之30。')

    async def test_relative_changes_use_live_value_and_ten_point_steps_with_clamps(self):
        for query, current, target in [('大一点', 30, 40), ('小一点', 30, 20),
                                       ('大一点', 95, 100), ('小一点', 5, 0), ('静音', 40, 0)]:
            planner = Planner([tool(GET, {}), tool(SET, {"volume": 77})])
            async def execute(name, values):
                if name == GET: return {"ok": True, "volume": current}
                self.assertEqual(values['volume'], target)
                return {"ok": True, "volume": values['volume']}
            answer = await answer_with_tools(planner, [{'role': 'user', 'content': query}], execute)
            self.assertIn(str(target), answer)

    def test_arguments_reject_bool_range_extra_properties_and_unknown_tools(self):
        for name, values in [(SET, {"volume": True}), (SET, {"volume": -1}), (SET, {"volume": 101}),
                             (SET, {"volume": 1.5}), (GET, {"volume": 30}), ("arbitrary", {})]:
            with self.assertRaises(ValueError): arguments(name, json.dumps(values))
        for result in ({"ok": False, "volume": 30}, {"ok": True, "volume": True}, {"ok": True, "volume": 101}):
            with self.assertRaises(ValueError): result_volume(result)

    async def test_query_returns_current_device_result_instead_of_hallucinated_content(self):
        planner = Planner([tool(GET, {}), {"content": "我猜是百分之九十"}])
        calls = []
        async def execute(name, values):
            calls.append((name, values))
            return {"ok": True, "volume": 30}
        answer = await answer_with_tools(planner, [{"role": "user", "content": "现在音量多少"}], execute)
        self.assertEqual(answer, "当前对话音量是百分之30。")
        self.assertEqual(calls, [(GET, {})])
        self.assertEqual(planner.requests[0]['tool_choice']['function']['name'], MODEL_GET)

    async def test_setting_and_muting_require_query_and_exact_device_ack(self):
        for target in (0, 30, 100):
            planner = Planner([tool(GET, {}), tool(SET, {"volume": target})])
            calls = []
            async def execute(name, values):
                calls.append(name)
                return {"ok": True, "volume": values.get('volume', 50)}
            answer = await answer_with_tools(planner, [{"role": "user", "content": "音量调到30"}], execute)
            self.assertEqual(calls, [GET, SET])
            self.assertIn(str(target), answer)

    async def test_failed_or_mismatched_device_ack_cannot_claim_success(self):
        for ack in ({"ok": False, "volume": 50}, {"ok": True, "volume": 60}):
            planner = Planner([tool(GET, {}), tool(SET, {"volume": 30})])
            async def execute(name, values): return {"ok": True, "volume": 50} if name == GET else ack
            with self.assertRaises(ValueError):
                await answer_with_tools(planner, [{"role": "user", "content": "调音量"}], execute)

    async def test_direct_setting_without_query_is_rejected(self):
        planner = Planner([tool(SET, {"volume": 30})])
        execute = mock.AsyncMock()
        with self.assertRaises(ValueError):
            await answer_with_tools(planner, [{"role": "user", "content": "大一点"}], execute)
        execute.assert_not_called()


@unittest.skipUnless(NATIVE_OPUS, "Requires native libopus")
class ToolTransportTests(unittest.IsolatedAsyncioTestCase):
    asyncTearDown = baseline.SessionTests.asyncTearDown
    start = baseline.SessionTests.start
    upload = baseline.SessionTests.upload
    until = baseline.SessionTests.until
    drain = baseline.SessionTests.drain
    async def asyncSetUp(self):
        await baseline.SessionTests.asyncSetUp(self)
        self.model_requests = []
        async def completion(body):
            self.model_requests.append(body)
            if body.get('tool_choice'):
                return tool(GET, {})
            last = body['messages'][-1]
            if last['role'] == 'tool':
                return tool(SET, {"volume": 30})
            return {"content": "你好"}
        self.provider.completion_message = completion
        self.provider.settings = {}
        self.provider.content_text = DashScope.content_text
        self.provider.answer_text = lambda message: message['content']
        self.provider.answer = mock.AsyncMock(return_value='你好')
        self.provider.recognize = mock.AsyncMock(side_effect=["音量调到30", "你好", "你好"])

    async def controls_client(self):
        from websockets.asyncio.client import connect
        from voice_server import AUDIO_PARAMS
        headers = {"Device-Id": "aa:bb:cc:dd:ee:01", "Authorization": "Bearer " + "0"*64, "Protocol-Version": "1"}
        socket = await self.stack.enter_async_context(connect(self.url, additional_headers=headers))
        await socket.send(json.dumps({"type": "hello", "transport": "websocket", "ai_protocol": 1,
            "reply_stream": 1, "max_reply_packet_bytes": 1275, "device_controls": 1, "volume_restore": 1, "audio_params": AUDIO_PARAMS}))
        hello = json.loads(await socket.recv())
        self.assertEqual(hello['device_controls'], 1)
        self.assertEqual(hello['volume_restore'], 1)
        return socket, hello['session_id']

    async def test_real_websocket_query_then_short_setting_keeps_original_history(self):
        self.provider.recognize = mock.AsyncMock(side_effect=['现在音量多少？','调到100。'])
        async def plan(body):
            return tool(GET,{}) if body.get('tool_choice') else tool(SET,{'volume':100})
        self.provider.completion_message = plan
        socket, session = await self.controls_client()
        await self.upload(socket,1)
        request = (await self.until(socket,'device_tool'))[-1]
        await self.ack(socket,request,50)
        await self.until(socket,'reply_end')
        await self.upload(socket,2)
        request = (await self.until(socket,'device_tool'))[-1]
        self.assertEqual(request['name'],GET)
        await self.ack(socket,request,50)
        request = (await self.until(socket,'device_tool'))[-1]
        self.assertEqual((request['name'],request['volume']),(SET,100))
        await self.ack(socket,request,100)
        messages = await self.until(socket,'reply_end')
        self.assertIn('百分之100', next(m['text'] for m in messages if m['type']=='reply_text'))
        self.assertEqual(self.service.histories['aa:bb:cc:dd:ee:01'].pairs[-1],
                         ('调到100。','已将对话音量调整到百分之100。'))

    async def ack(self, socket, message, volume, ok=True, **extra):
        await socket.send(json.dumps({"type": "tool_result", "turn": message['turn'],
            "call_id": message['call_id'], "session_id": message['session_id'], "ok": ok, "volume": volume, **extra}))

    async def test_real_websocket_query_set_ack_then_tts_and_single_history_pair(self):
        spoken = []
        original = self.provider.synthesize_stream
        async def capture(text):
            spoken.append(text)
            async for chunk in original(text): yield chunk
        self.provider.synthesize_stream = capture
        socket, session = await self.controls_client()
        await self.upload(socket, 1)
        first = (await self.until(socket, 'device_tool'))[-1]
        self.assertEqual(first['name'], GET)
        await self.ack(socket, first, 50)
        second = (await self.until(socket, 'device_tool'))[-1]
        self.assertEqual((second['name'], second['volume']), (SET, 30))
        await self.ack(socket, first, 50)  # Duplicate old acknowledgement cannot complete a new call.
        await self.ack(socket, second, 30)
        messages = await self.until(socket, 'reply_end')
        self.assertIn('百分之30', next(m['text'] for m in messages if m['type'] == 'reply_text'))
        history = self.service.histories['aa:bb:cc:dd:ee:01']
        self.assertEqual(len(history.pairs), 1)
        self.assertEqual(spoken, ['已将对话音量调整到百分之三十。'])
        await socket.send('{"type":"playback_done","turn":1}')
        await self.drain(socket)
        self.assertEqual(len(history.pairs), 1)

    async def test_cancel_before_ack_ignores_old_result_and_allows_next_ptt(self):
        socket, session = await self.controls_client()
        await self.upload(socket, 1)
        request = (await self.until(socket, 'device_tool'))[-1]
        await socket.send('{"type":"abort","turn":1}')
        await self.upload(socket, 2)
        await self.ack(socket, request, 100)
        messages = await self.until(socket, 'reply_end')
        self.assertTrue(all(m.get('turn') == 2 for m in messages))
        await socket.send('{"type":"playback_done","turn":2}')
        await self.drain(socket)
        self.assertEqual(list(self.service.histories['aa:bb:cc:dd:ee:01'].pairs),
                         [('你好', '你好呀，我是思巴迪！今天想聊什么？')])

    async def test_failed_device_write_emits_error_and_no_tts_or_history(self):
        socket, session = await self.controls_client()
        await self.upload(socket, 1)
        request = (await self.until(socket, 'device_tool'))[-1]
        await self.ack(socket, request, 50)
        request = (await self.until(socket, 'device_tool'))[-1]
        with self.assertLogs('stubuddy.ai', level='WARNING'):
            await self.ack(socket, request, 50, ok=False)
            messages = await self.until(socket, 'turn_error')
        self.assertFalse(any(m['type'] == 'reply_text' for m in messages))
        self.assertFalse(self.service.histories['aa:bb:cc:dd:ee:01'].pairs)

    async def test_successful_setting_is_not_rolled_back_when_reply_is_cancelled(self):
        socket, session = await self.controls_client()
        await self.upload(socket, 1)
        request = (await self.until(socket, 'device_tool'))[-1]
        await self.ack(socket, request, 50)
        request = (await self.until(socket, 'device_tool'))[-1]
        applied_volume = request['volume']
        await self.ack(socket, request, applied_volume)
        await self.until(socket, 'reply_end')
        await socket.send('{"type":"abort","turn":1}')
        await self.drain(socket)
        self.assertEqual(applied_volume, 30)
        self.assertEqual(list(self.service.histories['aa:bb:cc:dd:ee:01'].pairs),
                         [('音量调到30', '已将对话音量调整到百分之30。')])

    async def test_wrong_session_ack_is_rejected(self):
        from websockets.exceptions import ConnectionClosed
        socket, session = await self.controls_client()
        await self.upload(socket, 1)
        request = (await self.until(socket, 'device_tool'))[-1]
        await self.ack(socket, request, 50, session_id='0'*16)
        with self.assertRaises(ConnectionClosed):
            await socket.recv()
        self.assertFalse(self.service.histories['aa:bb:cc:dd:ee:01'].pairs)

    async def test_math_then_followup_over_real_socket_cannot_change_volume(self):
        self.provider.recognize = mock.AsyncMock(side_effect=['1+1','再加1'])
        self.provider.answer = mock.AsyncMock(side_effect=['2','3'])
        socket, _ = await self.controls_client()
        for turn, answer in [(1,'2'),(2,'3')]:
            await self.upload(socket,turn)
            messages = await self.until(socket,'reply_end')
            self.assertFalse(any(m['type']=='device_tool' for m in messages))
            self.assertEqual(next(m['text'] for m in messages if m['type']=='reply_text'), answer)
            await socket.send(json.dumps({'type':'playback_done','turn':turn}))
            await self.drain(socket)
        self.assertFalse(self.model_requests)
        sent = self.provider.answer.await_args.args[0]
        self.assertEqual([m['content'] for m in sent[1:]], ['1+1','2','再加1'])

    async def test_restore_wire_requests_real_device_restore_operation(self):
        self.provider.recognize = mock.AsyncMock(return_value='恢复音量')
        socket, _ = await self.controls_client()
        await self.upload(socket,1)
        query = (await self.until(socket,'device_tool'))[-1]
        await self.ack(socket,query,0)
        restore = (await self.until(socket,'device_tool'))[-1]
        self.assertEqual(restore['name'],RESTORE)
        self.assertNotIn('volume',restore)
        await self.ack(socket,restore,30)
        messages = await self.until(socket,'reply_end')
        self.assertEqual(next(m['text'] for m in messages if m['type']=='reply_text'), '已将对话音量恢复到百分之30。')


if __name__ == '__main__':
    unittest.main()
