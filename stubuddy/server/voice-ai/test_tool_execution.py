import asyncio
import unittest
from unittest.mock import AsyncMock
from tool_registry import catalog, definitions, validate, input_schema, ToolError
from tool_execution import execute_tool, create_reminder


class ToolExecutionTests(unittest.IsolatedAsyncioTestCase):
    async def test_discovery_and_strict_arguments(self):
        self.assertEqual(len(catalog({'device_controls', 'volume_restore', 'scheduled'})), 8)
        self.assertEqual(catalog(set()), ())
        self.assertEqual(len(definitions({"device_controls", "volume_restore", "scheduled"})), 8)
        for name, values in [('bad', {}), ('self.audio_speaker.set_volume', {'volume': True}),
                             ('self.audio_speaker.set_volume', {'volume': 101}),
                             ('self.get_device_status', {'device_id': 'other'}),
                             ('self.reminders.cancel', {'cache_id': 'invalid'})]:
            with self.assertRaises(ToolError):
                validate(name, values)
        self.assertFalse(input_schema('self.get_device_status')['additionalProperties'])

    async def test_unsupported_never_dispatches(self):
        transport = AsyncMock()
        with self.assertRaisesRegex(ToolError, 'unsupported_capability'):
            await execute_tool('self.reminders.get', {}, transport, set(), 'test', 1)
        transport.assert_not_awaited()

    async def test_timeout_is_uncertain_and_never_retries(self):
        for name, values, expected in [('self.get_device_status', {}, 'read_timeout'),
                ('self.audio_speaker.set_volume', {'volume': 50}, 'write_result_unknown')]:
            transport = AsyncMock(side_effect=TimeoutError)
            with self.assertLogs('stubuddy.ai', level='INFO') as logs:
                with self.assertRaisesRegex(ToolError, expected):
                    await execute_tool(name, values, transport, {'device_controls'}, 'test', 2)
            self.assertEqual(transport.await_count, 1)
            self.assertIn('elapsed_ms=', logs.output[0])
            self.assertIn(expected, logs.output[0])

    async def test_cancel_propagates_and_malformed_receipt_is_rejected(self):
        for response, error in [(asyncio.CancelledError(), asyncio.CancelledError), ({'ok': True}, ToolError)]:
            transport = AsyncMock(side_effect=response) if isinstance(response, BaseException) else AsyncMock(return_value=response)
            with self.assertRaises(error):
                await execute_tool('self.get_device_status', {}, transport, {'device_controls'}, 'test', 3)

    async def test_prepare_failure_prevents_device_write(self):
        values = {'cache_id': 'a' * 16, 'item': '喝水', 'text': '时间到啦。', 'seconds': 60}
        for failure in (RuntimeError('prepare failed'), asyncio.CancelledError()):
            execute = AsyncMock()
            with self.assertRaises(type(failure)):
                await create_reminder('self.reminders.countdown', values, AsyncMock(side_effect=failure), execute)
            execute.assert_not_awaited()
        prepare = AsyncMock()
        with self.assertRaises(ToolError):
            await create_reminder('self.reminders.countdown', dict(values, seconds=30), prepare, AsyncMock())
        prepare.assert_not_awaited()

    async def test_complete_creation_and_receipt_mismatch(self):
        from test_reminders import status, row
        values = {'cache_id': 'a' * 16, 'item': '喝水', 'text': '喝水时间到啦。', 'seconds': 60}
        events = []
        async def prepare(*args): events.append('prepare')
        async def execute(*args):
            events.append('write')
            return status([row('a', 'countdown', 1)])
        await create_reminder('self.reminders.countdown', values, prepare, execute)
        self.assertEqual(events, ['prepare', 'write'])
        with self.assertRaisesRegex(ToolError, 'creation_receipt_mismatch'):
            await create_reminder('self.reminders.countdown', values, prepare, AsyncMock(return_value=status([])))

    async def test_device_business_rejection_is_preserved(self):
        from test_reminders import status
        result = status(ok=False, error=1)
        with self.assertLogs('stubuddy.ai', level='INFO') as logs:
            actual = await execute_tool('self.reminders.get', {}, AsyncMock(return_value=result), {'scheduled'}, 'test', 1)
        self.assertEqual(actual, result)
        self.assertIn('outcome=capacity', logs.output[0])


    async def test_success_flag_cannot_hide_setting_mismatch(self):
        with self.assertRaisesRegex(ToolError, 'setting_receipt_mismatch'):
            await execute_tool('self.audio_speaker.set_volume', {'volume': 60},
                AsyncMock(return_value={'ok': True, 'volume': 50}), {'device_controls'}, 'test', 1)
