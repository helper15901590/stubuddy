"""Shared execution boundary for voice and future trusted adapters.

Adapters must bind authenticated device/session/turn themselves. They must not
accept model-supplied device identity or expose raw reminder cache operations.
"""
import asyncio
import logging
import secrets
import time
from websockets.exceptions import ConnectionClosed
from tool_registry import validate, ToolError

LOG = logging.getLogger('stubuddy.ai')


async def execute_tool(name, values, transport, capabilities, session, turn):
    started = time.monotonic()
    operation = secrets.token_hex(8)
    outcome = 'internal_error'
    tool = None
    try:
        tool = validate(name, values, capabilities)
        result = await transport(name, dict(values))
        if name.startswith('self.reminders.'):
            from reminder_tools import validate_status
            validate_status(result)
        elif name.startswith('self.countdown.'):
            from countdown_tools import validate_status
            validate_status(result)
        else:
            from device_tools import result_volume
            result_volume(result)
        if result['ok']:
            if name.endswith('.set_volume') and result['volume'] != values['volume']:
                raise ToolError('setting_receipt_mismatch')
            if name.endswith('.restore_volume') and result['volume'] == 0:
                raise ToolError('setting_receipt_mismatch')
            if name.endswith('.volume') and result['alarm_volume'] != values['volume']:
                raise ToolError('setting_receipt_mismatch')
            if name == 'self.reminders.cancel' and any(row['id'] == values['cache_id'] for row in result['records']):
                raise ToolError('cancellation_receipt_mismatch')
            if name == 'self.countdown.cancel' and result['active']:
                raise ToolError('cancellation_receipt_mismatch')
        outcome = ('confirmed' if result['ok'] else
                   {1: 'capacity', 2: 'spacing', 3: 'not_ready'}.get(result.get('error'), 'device_rejected'))
        return result  # Keep accepted business rejection replies unchanged.
    except asyncio.CancelledError:
        outcome = 'cancelled'
        raise
    except TimeoutError as error:
        outcome = 'read_timeout' if tool and tool.read_only else 'write_result_unknown'
        raise ToolError(outcome) from error
    except ToolError as error:
        outcome = error.code
        raise
    except ValueError as error:
        outcome = 'invalid_receipt'
        raise ToolError(outcome) from error
    except ConnectionClosed:
        outcome = 'transport_failure' if tool and tool.read_only else 'write_result_unknown'
        raise
    except (ConnectionError, OSError) as error:
        outcome = 'transport_failure' if tool and tool.read_only else 'write_result_unknown'
        raise ToolError(outcome) from error
    finally:
        LOG.info('TOOL EXEC session=%s turn=%s operation=%s name=%s outcome=%s elapsed_ms=%s',
                 session, turn, operation, name if tool else 'unvalidated', outcome,
                 round((time.monotonic() - started) * 1000))


async def create_reminder(name, values, prepare, execute):
    """Complete creation sequence; never write before voice preparation succeeds."""
    if name not in ('self.reminders.start', 'self.reminders.countdown', 'self.countdown.start'):
        raise ToolError('invalid_creation_tool')
    validate(name, values)
    started = time.monotonic()
    try:
        await prepare(values['text'], values['cache_id'])
    finally:
        LOG.info('TOOL PREPARE name=%s elapsed_ms=%s', name, round((time.monotonic() - started) * 1000))
    result = await execute(name, values)
    if name.startswith('self.reminders.'):
        from reminder_tools import validate_status
        validate_status(result)
        if not result['ok']:
            return result
        row = next((row for row in result['records'] if row['id'] == values['cache_id']), None)
        expected_kind = 'countdown' if name.endswith('.countdown') else 'scheduled'
        if not row or row['kind'] != expected_kind or any(row[key] != values[key] for key in ('item', 'text')):
            raise ToolError('creation_receipt_mismatch')
        if 'seconds' in values:
            if not result['now'] + values['seconds'] - 5 <= row['deadline'] <= result['now'] + values['seconds']:
                raise ToolError('creation_receipt_mismatch')
        elif any(row[key] != values[key] for key in ('deadline', 'minute', 'weekdays')):
            raise ToolError('creation_receipt_mismatch')
    else:
        from countdown_tools import validate_status
        validate_status(result)
        if not result['active'] or not values['seconds'] - 5 <= result['remaining'] <= values['seconds']:
            raise ToolError('creation_receipt_mismatch')
    return result
