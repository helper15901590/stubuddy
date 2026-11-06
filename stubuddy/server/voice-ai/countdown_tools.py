"""One device-owned countdown; prepared voice and acknowledged tools only."""
from prompt_store import wording
import base64
import json
import re
import secrets
import zlib
from decimal import Decimal, InvalidOperation

from audio import encode
from device_tools import chinese_volume, short_setting, short_restore
from providers import clean_spoken_text

GET = "self.countdown.get"
START = "self.countdown.start"
CANCEL = "self.countdown.cancel"
VOLUME = "self.countdown.volume"
RANGE_REPLY = wording('countdown_tools.reply.global.001')


def normalize(text):
    # ASR ITN may emit 30s / 1min / 0.5h, without a space before Chinese.
    for pattern, unit in ((r'(?<=\d)\s*(?:seconds?|secs?|s)(?![a-z])', '秒'),
                          (r'(?<=\d)\s*(?:minutes?|mins?|min|m)(?![a-z])', '分钟'),
                          (r'(?<=\d)\s*(?:hours?|hrs?|hr|h)(?![a-z])', '小时')):
        text = re.sub(pattern, unit, text, flags=re.I)
    return text


NUMBER = r'[负零〇一二两三四五六七八九十百千万\d.点半个+-]+'
DURATION = re.compile('(' + NUMBER + r')\s*(小时|钟头|分钟|秒钟|秒|分)(半)?')


def number(text):
    text = text.replace('个', '').replace('两', '二').replace('〇', '零')
    negative = text.startswith(('负', '-'))
    text = text.lstrip('负-+')
    if text.endswith('半'):
        return (-1 if negative else 1) * ((number(text[:-1]) if text[:-1] else Decimal(0)) + Decimal('0.5'))
    try:
        value = Decimal(text)
    except InvalidOperation:
        digits = {word: index for index, word in enumerate('零一二三四五六七八九')}
        if '点' in text:
            whole, fraction = text.split('点', 1)
            if not fraction or any(c not in digits and not c.isdigit() for c in fraction):
                raise ValueError('Invalid fractional duration')
            value = number(whole or '零') + Decimal('0.' + ''.join(str(digits[c]) if c in digits else c for c in fraction))
        else:
            value, current = 0, 0
            for word in text:
                if word in digits: current = digits[word]
                elif word in '十百千万':
                    unit = {'十': 10, '百': 100, '千': 1000, '万': 10000}[word]
                    if unit == 10000: value, current = (value + current) * unit, 0
                    else: value, current = value + (current or 1) * unit, 0
                else: raise ValueError('Invalid duration number')
            value = Decimal(value + current)
    return -value if negative else value


def duration_seconds(text):
    text = normalize(text)
    matches = list(DURATION.finditer(text))
    if not matches:
        return None
    # Units after the deadline may describe the activity, not the countdown.
    if any('后' in text[a.end():b.start()] for a, b in zip(matches, matches[1:])):
        # The reminder's activity can contain another duration; it mustn't
        # enlarge an explicitly too-short countdown (30秒后做一分钟操).
        after = text.index('后')
        matches = [match for match in matches if match.end() <= after]
    total = Decimal(0)
    try:
        for match in matches:
            unit = 3600 if match[2] in ('小时', '钟头') else 60 if match[2] in ('分钟', '分') else 1
            total += (number(match[1]) + (Decimal('0.5') if match[3] else 0)) * unit
    except (ValueError, InvalidOperation):
        return None
    return total


def query_intent(text):
    return bool(re.search(r'还[有剩].*(?:多久|多长|多少|几)|剩余|查询|有没有|有哪些|有什么|什么倒计时', text))


def alarm_context(messages):
    return any(re.search(r'(?:提醒|闹钟)(?:的)?音量', m.get('content', ''))
               for m in (messages or [])[-3:-1])


def intent(text, messages=None):
    text = normalize(text)
    duration = DURATION.search(text)
    if re.search(r'倒计时|(?:提醒|闹钟)(?:的)?音量', text):
        return True
    if duration and (re.search(r'设置|设定|设个|设一个|帮我设|创建|开启|启动|计时|提醒|叫我', text) or
                     re.search(r'后.{0,12}(?:喝水|休息|吃饭|做作业)', text)):
        return True
    if '提醒' in text and (query_intent(text) or re.search(r'取消|删除|停止|关掉|关闭', text)):
        return True
    if alarm_context(messages) and (short_setting(text) or short_restore(text) or re.fullmatch(r'\s*(?:大一点|小一点|大声一点|小声一点)[。！!\s]*', text)):
        return True
    timer_context = any(re.search(r'倒计时|提醒|(?:设置|创建).*后', m.get('content', ''))
                        for m in (messages or [])[-3:-1])
    return bool(timer_context and (query_intent(text) or
                re.fullmatch(r'\s*(?:取消(?:它)?|删除(?:它)?)[。！？!?\s]*', text)))


def spoken_duration(seconds):
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    return "".join(chinese_volume(n) + unit for n, unit in
                   ((hours, "小时"), (minutes, "分钟"), (seconds, "秒")) if n)


async def prepare_voice(provider, text, identity, send):
    text = clean_spoken_text(text)
    if not isinstance(text, str) or not 1 <= len(text) <= 60:
        raise ValueError("Invalid reminder text")
    pcm = await provider.synthesize(text)
    if not pcm or len(pcm) % 2 or len(pcm) > 16000 * 2 * 12:
        raise ValueError("Reminder voice exceeds twelve seconds")
    pcm = pcm.ljust(9600, b"\0")
    packets, samples, delay = encode(pcm)  # Existing mature Opus, <=227 bytes.
    await send({"type": "countdown_cache", "id": identity, "state": "start"})
    crc = 0
    for sequence, packet in enumerate(packets):
        crc = zlib.crc32(packet, crc)
        await send({"type": "countdown_packet", "id": identity, "sequence": sequence,
                    "data": base64.b64encode(packet).decode("ascii")})
    await send({"type": "countdown_cache", "id": identity, "state": "end",
                "samples": samples, "delay": delay, "frames": len(packets), "crc32": crc})


async def answer(provider, messages, execute, prepare):
    query = normalize(messages[-1]["content"])
    status = await execute(GET, {})
    validate_status(status)
    if re.search(r"(?:提醒|闹钟)(?:的)?音量", query) or (alarm_context(messages) and
            (short_setting(query) or short_restore(query) or re.search(r'大一点|小一点|大声一点|小声一点', query))):
        if short_restore(query):
            return wording('countdown_tools.reply.answer.002')
        if not re.search(r"调|设|静音|大一点|小一点|大声|小声", query):
            return wording('countdown_tools.reply.answer.003', p0=(chinese_volume(status['alarm_volume'])))
        schema = {"volume": {"type": "integer", "minimum": 0, "maximum": 100}}
        instruction = (wording('countdown_tools.prompt.answer.004', p0=(status['alarm_volume'])))
        values = await plan(provider, messages, instruction, schema, ["volume"])
        volume = values.get("volume")
        if re.search(r'小一点|小声一点', query): volume = max(0, status['alarm_volume'] - 10)
        elif re.search(r'大一点|大声一点', query): volume = min(100, status['alarm_volume'] + 10)
        elif '静音' in query: volume = 0
        if type(volume) is not int or not 0 <= volume <= 100:
            raise ValueError("Invalid alarm volume")
        result = await execute(VOLUME, {"volume": volume})
        validate_status(result)
        if result['alarm_volume'] != volume:
            raise ValueError("Alarm volume acknowledgement mismatch")
        return wording('countdown_tools.reply.answer.005', p0=(chinese_volume(volume)))
    if re.search(r"取消|删除|停止|关掉|关闭", query):
        result = await execute(CANCEL, {})
        validate_status(result)
        if result['active']:
            raise ValueError("Countdown cancellation not confirmed")
        return wording('countdown_tools.reply.answer.006') if status['active'] else wording('countdown_tools.reply.answer.007')
    if query_intent(query):
        return (wording('countdown_tools.reply.answer.008', p0=(spoken_duration(max(1, status['remaining'])))) if status['active']
                else wording('countdown_tools.reply.answer.007'))
    seconds_from_query = duration_seconds(query)
    if seconds_from_query is not None and (not 60 <= seconds_from_query <= 86400 or seconds_from_query % 1):
        return RANGE_REPLY
    if status['active']:
        return wording('countdown_tools.reply.answer.009')
    values = await plan(provider, messages,
        wording('countdown_tools.prompt.answer.010'),
        {"seconds": {"type": "integer"}, "text": {"type": "string", "maxLength": 40}},
        ["seconds", "text"])
    seconds, text = values.get('seconds'), values.get('text')
    if seconds_from_query is not None:
        seconds = int(seconds_from_query)  # Explicit duration must never be silently clamped by the model.
    if type(seconds) is not int or not 60 <= seconds <= 86400:
        return RANGE_REPLY
    text = clean_spoken_text(text) if isinstance(text, str) else text
    if not isinstance(text, str) or not 1 <= len(text) <= 40 or len(text.encode('utf-8')) > 120:
        raise ValueError("Invalid reminder wording")
    identity = secrets.token_hex(8)
    await prepare(text, identity)
    result = await execute(START, {"seconds": seconds, "text": text, "cache_id": identity})
    validate_status(result)
    if not result['active'] or result['remaining'] > seconds or result['remaining'] < seconds - 5:
        raise ValueError("Countdown start not confirmed")
    return wording('countdown_tools.reply.answer.011', p0=(spoken_duration(seconds)))


def validate_status(result):
    if (not isinstance(result, dict) or result.get('ok') is not True or
            type(result.get('active')) is not bool or type(result.get('remaining')) is not int or
            not 0 <= result['remaining'] <= 86400 or type(result.get('alarm_volume')) is not int or
            not 0 <= result['alarm_volume'] <= 100):
        raise ValueError("Device did not confirm countdown status")


class PlanValidationError(ValueError):
    """The model returned no usable structured plan; no device write occurred."""


async def plan(provider, messages, instruction, properties, required, max_tokens=256):
    conversation = [dict(m) for m in messages]
    conversation.insert(0, {"role": "system", "content": instruction})
    message = await provider.completion_message({
        "model": provider.settings.get('llm_model', 'qwen-plus'), "messages": conversation,
        "tools": [{"type": "function", "function": {"name": "countdown_plan",
            "description": instruction, "parameters": {"type": "object", "properties": properties,
            "required": required, "additionalProperties": False}}}],
        "tool_choice": {"type": "function", "function": {"name": "countdown_plan"}},
        "parallel_tool_calls": False, "stream": False, "enable_thinking": False,
        "temperature": 0.2, "max_tokens": max_tokens})
    calls = message.get('tool_calls')
    if not isinstance(calls, list) or len(calls) != 1 or not isinstance(calls[0], dict):
        raise PlanValidationError("Expected one countdown plan")
    function = calls[0].get('function', {})
    if not isinstance(function, dict):
        raise PlanValidationError('Invalid countdown function')
    raw = function.get('arguments')
    if function.get('name') != 'countdown_plan' or not isinstance(raw, str) or len(raw) > 600:
        raise PlanValidationError("Invalid countdown plan")
    try:
        values = json.loads(raw)
    except json.JSONDecodeError as error:
        raise PlanValidationError('Malformed countdown arguments') from error
    if type(values) is not dict or set(values) != set(required):
        raise PlanValidationError("Invalid countdown arguments")
    return values
