"""Spoken help from negotiated capabilities, never a device operation."""
from prompt_store import wording
import re


def answer(query, controls=False, restore=False, countdown=False, scheduled=False):
    text = re.sub(r'[\s，,。.!！?？]', '', query)
    general = bool(re.search(r'(?:有(?:哪些|什么|多少个?)|能用什么)(?:功能|工具|本领)|(?:能|可以|会)(?:做什么|干什么|做些什么)', text))
    general = general and bool(re.search(r'你|思巴迪|设备|伴学机|功能介绍|工具列表|^(?:请问)?(?:有|能|可以|会)', text))
    reminder = bool(re.search(r'(?:提醒|倒计时|闹钟)(?:功能)?(?:怎么用|如何用|有什么功能)|你(?:能|可以|支持)(?:设置)?(?:提醒|倒计时|闹钟)吗', text))
    volume = bool(re.search(r'(?:音量|静音)(?:功能)?(?:怎么用|如何用)|你(?:能|可以|支持)(?:调节|调整|查询)(?:音量|声音)吗', text))
    if not (general or reminder or volume):
        return None
    if reminder:
        if scheduled:
            return (wording('capabilities.reply.answer.001'))
        if countdown:
            return wording('capabilities.reply.answer.002')
        return wording('capabilities.reply.answer.003')
    if volume:
        if not controls:
            return wording('capabilities.reply.answer.004')
        return (wording('capabilities.reply.answer.005') +
                (wording('capabilities.reply.answer.006') if restore else '') +
                (wording('capabilities.reply.answer.007') if countdown else ''))
    parts = [wording('capabilities.reply.introduction')]
    if controls:
        parts.append(wording('capabilities.reply.answer.008') + (wording('capabilities.reply.answer.009') if restore else '。'))
    if scheduled:
        parts.append(wording('capabilities.reply.answer.010'))
    elif countdown:
        parts.append(wording('capabilities.reply.answer.011'))
    parts.append(wording('capabilities.reply.answer.012'))
    return ''.join(parts)
