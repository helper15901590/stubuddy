"""Restricted volume tools; device acknowledgement is the source of truth."""
import json
import re

GET = "self.get_device_status"
SET = "self.audio_speaker.set_volume"
RESTORE = "self.audio_speaker.restore_volume"
MODEL_GET = "self_get_device_status"
MODEL_SET = "self_audio_speaker_set_volume"
MODEL_RESTORE = "self_audio_speaker_restore_volume"
MODEL_NAMES = {MODEL_GET: GET, MODEL_SET: SET, MODEL_RESTORE: RESTORE}
TOOLS = [
    {"type": "function", "function": {"name": MODEL_GET,
        "description": "查询本设备当前真实对话音量。每次查询或调整前都要重新读取，不能从历史猜测。",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False}}},
    {"type": "function", "function": {"name": MODEL_SET,
        "description": "设置对话音量0..100，0静音。大一点/小一点基于刚查询的值加/减10，夹到0..100。",
        "parameters": {"type": "object", "properties": {"volume": {"type": "integer", "minimum": 0, "maximum": 100}},
                       "required": ["volume"], "additionalProperties": False}}},
    {"type": "function", "function": {"name": MODEL_RESTORE,
        "description": "恢复本设备保存的静音前音量，不从历史猜测。",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False}}}
]
INSTRUCTION = ("你可以查询和调整这台设备的对话音量。音量操作必须调用工具，不能声称已执行未执行的操作。"
               "必须先查当前值再设置；大一点加10，小一点减10，边界为0..100。每轮最多设置一次。"
               "100为现有最大输出。只有设备确认才代表成功。与设备音量无关的问题正常回答。")


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
    if value == 100: return "一百"
    if value < 10: return digits[value]
    tens, units = divmod(value, 10)
    return (digits[tens] if tens > 1 else "") + "十" + (digits[units] if units else "")


def volume_speech_text(text):
    # Only our exact device-confirmed templates; never rewrite general math.
    match = re.fullmatch(r"(当前对话音量是|已将对话音量(?:调整|恢复)到)百分之([0-9]{1,3})。", text)
    if match and int(match[2]) <= 100:
        return match[1] + "百分之" + chinese_volume(int(match[2])) + "。"
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
    if type(value) is not dict or set(value) != ({"volume"} if name == SET else set()):
        raise ValueError("Invalid tool arguments")
    if name == SET and (type(value["volume"]) is not int or not 0 <= value["volume"] <= 100):
        raise ValueError("Invalid volume")
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
            return "你是想调节音量吗？请说音量调到具体数值。"
        messages = [dict(message) for message in messages]
        messages[-1]["content"] = query = "音量" + query.strip()
    if short_restore(query):
        if not restore_context(messages):
            return "你是想恢复音量吗？请说恢复音量。"
        messages = [dict(message) for message in messages]
        messages[-1]["content"] = query = "恢复音量"
    query_only = not re.search(r"调|设|恢复|还原|静音|大一点|小一点|大声一点|小声一点", query)
    if not volume_intent(query):
        if re.fullmatch(r"\s*(?:你好|您好|嗨|哈[喽啰咯]|hello|hi)[呀啊哦吗。！？!?.,，\s]*", query, re.I):
            return "你好呀，我是思巴迪！今天想聊什么？"
        # Do not expose device tools on a learning/chat turn. In particular,
        # arithmetic follow-ups must not inherit the last device-control topic.
        plain = [dict(message) for message in messages]
        instruction = ("本轮是普通问答，不能操作设备或声称改变了音量。"
                       "‘再加1’等算术追问延续最近的数学问答；缺少对应历史时先澄清，不把数字当音量。")
        if plain and plain[0].get("role") == "system": plain[0]["content"] += "\n" + instruction
        else: plain.insert(0, {"role": "system", "content": instruction})
        return await provider.answer(plain)
    if restore_intent(query) and not restore_supported:
        return "当前固件尚不支持恢复上次音量，请指定数值，例如音量调到50。"
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
                return f"当前对话音量是百分之{current}。"
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
            return f"已将对话音量{'恢复' if name == RESTORE else '调整'}到百分之{volume}。"
        current = volume
        if query_only:
            return f"当前对话音量是百分之{current}。"
        if restore_intent(query):
            restored = result_volume(await execute(RESTORE, {}))
            if not restored: raise ValueError("Device restore remained muted")
            return f"已将对话音量恢复到百分之{restored}。"
        conversation += [{"role": "assistant", "content": message.get("content"), "tool_calls": calls},
                         {"role": "tool", "tool_call_id": call["id"],
                          "content": json.dumps({"ok": True, "audio_speaker": {"volume": current}}, ensure_ascii=False)}]
    raise ValueError("Volume tool limit exceeded")
