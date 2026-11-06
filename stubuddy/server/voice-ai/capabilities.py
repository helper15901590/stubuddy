"""Spoken help from negotiated capabilities, never a device operation."""
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
            return ('可以设置一个一分钟到二十四小时的倒计时，以及最多三个一次性或每周指定星期提醒。'
                    '例如，每周一晚上八点提醒我看书。你也可以查询提醒，或说取消第一条提醒；提醒音量可以单独调节。')
        if countdown:
            return '可以设置一个一分钟到二十四小时的倒计时，例如提醒我十分钟后喝水，也能查询、取消和单独调节提醒音量。'
        return '当前设备还没有开放提醒功能，可以先和我语音聊天或一起学习。'
    if volume:
        if not controls:
            return '当前设备还没有开放语音调节音量。'
        return ('可以问当前音量，或说音量调到五十、大一点、小一点、静音。' +
                ('说恢复音量，可以恢复静音前的音量。' if restore else '') +
                ('提醒音量可以单独设置。' if countdown else ''))
    parts = ['我能陪你语音聊天、讲故事、解释知识，一起练习学习题目。']
    if controls:
        parts.append('还能查询、调节或静音对话音量' + ('，并恢复静音前音量。' if restore else '。'))
    if scheduled:
        parts.append('可以设置一个倒计时和最多三个一次性或每周提醒，查询、逐条取消，并单独调节提醒音量。')
    elif countdown:
        parts.append('可以设置、查询、取消一个倒计时，并单独调节提醒音量。')
    parts.append('按住按键说话，松开发送；播报时重新按键可以打断。')
    return ''.join(parts)
