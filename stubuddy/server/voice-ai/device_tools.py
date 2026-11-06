"""Restricted volume tools; device acknowledgement is the source of truth."""
from prompt_store import wording
import json
import re
from tool_registry import input_schema, validate

GET = "self.get_device_status"
SET = "self.audio_speaker.set_volume"
RESTORE = "self.audio_speaker.restore_volume"
MODEL_GET = "self_get_device_status"
MODEL_SET = "self_audio_speaker_set_volume"
MODEL_RESTORE = "self_audio_speaker_restore_volume"
MODEL_NAMES = {MODEL_GET: GET, MODEL_SET: SET, MODEL_RESTORE: RESTORE}
TOOLS = [
    {'type': 'function', 'function': {
        'name': model, 'description': wording(description), 'parameters': input_schema(name)}}
    for model, name, description in (
        (MODEL_GET, GET, 'device_tools.prompt.global.001'),
        (MODEL_SET, SET, 'device_tools.prompt.global.002'),
        (MODEL_RESTORE, RESTORE, 'device_tools.prompt.global.003'))
]
INSTRUCTION = (wording('device_tools.prompt.global.004'))


def restore_intent(text):
    if re.search(r"[0-9]|(?:到|为|成|百分之)\s*[零一二两三四五六七八九十百]", text):
        return False
    return bool(re.search(r"(?:恢复|还原).{0,10}(?:音量|声音)|(?:音量|声音).{0,5}(?:恢复|还原)|(?:取消|解除|退出)静音", text))


def short_restore(text):
    return bool(re.fullmatch(r"\s*恢复(?:一下|吧|一下吧)?[。！？!?．.，,\s]*", text))


def short_setting(text):
    return bool(re.fullmatch(r"\s*(?:请)?(?:再)?(?:调|调整|设|设置)(?:到|成|为)\s*(?:百分之)?"
                             r"(?:[0-9]{1,3}|[零一二两三四五六七八九十百]{1,5})\s*[%％]?[。！？!?．.，,\s]*", text))


def chinese_volume(value):
    if type(value) is not int or not 0 <= value <= 100:
        raise ValueError("Invalid spoken volume")
    digits = "零一二三四五六七八九"
    if value == 100: return wording('device_tools.reply.chinese_volume.005')
    if value < 10: return digits[value]
    tens, units = divmod(value, 10)
    return (digits[tens] if tens > 1 else "") + wording('device_tools.reply.chinese_volume.006') + (digits[units] if units else "")


def volume_speech_text(text):
    # Only our exact device-confirmed templates; never rewrite general math.
    match = re.fullmatch(r"(当前对话音量是|已将对话音量(?:调整|恢复)到)百分之([0-9]{1,3})。", text)
    if match and int(match[2]) <= 100:
        return match[1] + wording('device_tools.reply.volume_speech_text.007') + chinese_volume(int(match[2])) + "。"
    return text


def restore_context(messages):
    # Only the latest completed exchange supplies an implicit control topic.
    if len(messages) < 3:
        return False
    previous = messages[-3:-1]
    return any(re.search(r"音量|静音", message.get("content", "")) for message in previous)


def volume_intent(text, messages=None):
    if short_restore(text) or short_setting(text):
        return messages is not None and restore_context(messages)
    if restore_intent(text):
        return True
    return bool(re.search(r"(音量|声音).{0,10}(多少|多大|调|设|大一点|小一点|百分|[0-9])|"
                          r"(调|设).{0,10}(音量|声音)|大一点|小一点|大声一点|小声一点|静音", text))


def arguments(name, raw):
    if name not in (GET, SET, RESTORE) or not isinstance(raw, str) or len(raw) > 256:
        raise ValueError("Invalid tool call")
    value = json.loads(raw)
    validate(name, value)
    return value


def result_volume(result):
    if (type(result) is not dict or result.get("ok") is not True or
            type(result.get("volume")) is not int or not 0 <= result["volume"] <= 100):
        raise ValueError("Device did not confirm volume")
    return result["volume"]


async def answer_with_tools(provider, messages, execute, restore_supported=False):
    query = messages[-1]["content"]
    if short_setting(query):
        if not restore_context(messages):
            return wording('device_tools.reply.answer_with_tools.008')
        messages = [dict(message) for message in messages]
        messages[-1]["content"] = query = "音量" + query.strip()
    if short_restore(query):
        if not restore_context(messages):
            return wording('device_tools.reply.answer_with_tools.009')
        messages = [dict(message) for message in messages]
        messages[-1]["content"] = query = "恢复音量"
    query_only = not re.search(r"调|设|恢复|还原|静音|大一点|小一点|大声一点|小声一点", query)
    if not volume_intent(query):
        if re.fullmatch(r"\s*(?:你好|您好|嗨|哈[喽啰咯]|hello|hi)[呀啊哦吗。！？!?.,，\s]*", query, re.I):
            return wording('device_tools.reply.answer_with_tools.010')
        # Do not expose device tools on a learning/chat turn. In particular,
        # arithmetic follow-ups must not inherit the last device-control topic.
        plain = [dict(message) for message in messages]
        instruction = (wording('device_tools.prompt.answer_with_tools.011'))
        if plain and plain[0].get("role") == "system": plain[0]["content"] += "\n" + instruction
        else: plain.insert(0, {"role": "system", "content": instruction})
        return await provider.answer(plain)
    if restore_intent(query) and not restore_supported:
        return wording('device_tools.reply.answer_with_tools.012')
    conversation = [dict(message) for message in messages]
    if conversation and conversation[0].get("role") == "system":
        conversation[0]["content"] += "\n" + INSTRUCTION
    else:
        conversation.insert(0, {"role": "system", "content": INSTRUCTION})
    current = None
    for step in range(4):
        body = {"model": provider.settings.get("llm_model", "qwen-plus"),
                "messages": conversation, "tools": TOOLS[:1] if query_only else
                    TOOLS if restore_supported and restore_intent(query) else TOOLS[:2], "stream": False,
                "enable_thinking": False, "temperature": 0.2, "max_tokens": 256,
                "parallel_tool_calls": False}
        if step == 0 and volume_intent(messages[-1]["content"]):
            body["tool_choice"] = {"type": "function", "function": {"name": MODEL_GET}}
        message = await provider.completion_message(body)
        calls = message.get("tool_calls")
        if not calls:
            if current is not None:
                return wording('device_tools.reply.answer_with_tools.013', p0=(current))
            if volume_intent(messages[-1]["content"]):
                raise ValueError("Volume request requires a real tool result")
            return provider.answer_text(message)
        if not isinstance(calls, list) or len(calls) != 1:
            raise ValueError("Only one serial volume tool is allowed")
        call = calls[0]
        if (not isinstance(call, dict) or call.get("type") != "function" or
                not isinstance(call.get("id"), str) or not 1 <= len(call["id"]) <= 128):
            raise ValueError("Invalid tool identity")
        function = call.get("function", {})
        name = MODEL_NAMES.get(function.get("name"))
        values = arguments(name, function.get("arguments"))
        if name in (SET, RESTORE) and current is None:
            raise ValueError("Query the device before setting volume")
        if name == RESTORE and not restore_supported:
            raise ValueError("Restore capability not negotiated")
        if (name == SET and query_only) or (name == RESTORE and not restore_intent(query)):
            raise ValueError("Tool operation does not match current request")
        if name == SET:
            query = messages[-1]["content"]
            if re.search(r"小一点|小声一点", query): values["volume"] = max(0, current - 10)
            elif re.search(r"大一点|大声一点", query): values["volume"] = min(100, current + 10)
            elif "静音" in query and "取消静音" not in query: values["volume"] = 0
        result = await execute(name, values)
        volume = result_volume(result)
        if name in (SET, RESTORE):
            if name == SET and volume != values["volume"]:
                raise ValueError("Device setting mismatch")
            if name == RESTORE and volume == 0:
                raise ValueError("Device restore remained muted")
            return wording('device_tools.reply.answer_with_tools.014', p0=('恢复' if name == RESTORE else '调整'), p1=(volume))
        current = volume
        if query_only:
            return wording('device_tools.reply.answer_with_tools.013', p0=(current))
        if restore_intent(query):
            restored = result_volume(await execute(RESTORE, {}))
            if not restored: raise ValueError("Device restore remained muted")
            return wording('device_tools.reply.answer_with_tools.015', p0=(restored))
        conversation += [{"role": "assistant", "content": message.get("content"), "tool_calls": calls},
                         {"role": "tool", "tool_call_id": call["id"],
                          "content": json.dumps({"ok": True, "audio_speaker": {"volume": current}}, ensure_ascii=False)}]
    raise ValueError("Volume tool limit exceeded")
