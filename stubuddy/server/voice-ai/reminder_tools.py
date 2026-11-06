"""Bounded reminder tools. The device owns plans, deadlines and final validation."""
from tool_execution import create_reminder
from prompt_store import wording
from datetime import datetime, timedelta, timezone
import re
import secrets
import logging
import time

import countdown_tools as old
from device_tools import short_restore, short_setting, chinese_volume
from providers import clean_spoken_text
from calendar_input import normalize as normalize_calendar, CalendarInputError

TZ = timezone(timedelta(hours=8))
GET, START, TIMER, CANCEL, VOLUME = ('self.reminders.' + name for name in
                                   ('get', 'start', 'countdown', 'cancel', 'volume'))
MAX_TIME = 4102444800  # Device and server agree: before 2100 UTC.
LOG = logging.getLogger('stubuddy.ai')
DRAFT_SECONDS = 300
CLOCK_NUMBER = r'(?:\d{1,2}|[零〇一二两三四五六七八九十]{1,3})'
CLOCK_PATTERN = re.compile(r'(凌晨|清晨|早上|早晨|上午|中午|下午|傍晚|晚上|晚间|夜里|夜间)?\s*'
    '(' + CLOCK_NUMBER + r')(?:[:：](\d{1,2})(?!\d)|(?:点|时)(半|一刻|三刻|' + CLOCK_NUMBER + r'分?|整)?)')


def compact(text):
    return re.sub(r'[\s，,。.!！?？]', '', normalize_calendar(old.normalize(text)))


def live_draft(view):
    draft = view.get('draft')
    if draft and time.monotonic() < draft['expires']:
        return draft
    view.pop('draft', None)
    return None


def timer_without_subject(source):
    """Recognize only duration/request words; never discard a supplied activity."""
    remainder = old.DURATION.sub('', old.normalize(source))
    remainder = re.sub(r'补充[:：]|[\s，,。.!！?？]', '', remainder)
    return bool(re.fullmatch(r'(?:请|帮我|给我|麻烦|设置|设定|设|启动|开始|一个|个|倒计时|计时|提醒我|叫我|之后|以后|后|吧|一下|钟)*', remainder))


def clock_value(text, *, codes=False):
    """Only user-spoken clock evidence may supply a calendar time. None != 00:00."""
    try: text = normalize_calendar(text)
    except CalendarInputError: return None, ('reminder_tools.reply.clock_value.001' if codes else wording('reminder_tools.reply.clock_value.001'))
    matches = list(CLOCK_PATTERN.finditer(text))
    if not matches: return None, ('reminder_tools.reply.clock_value.002' if codes else wording('reminder_tools.reply.clock_value.002'))
    if len(matches) != 1: return None, ('reminder_tools.reply.clock_value.003' if codes else wording('reminder_tools.reply.clock_value.003'))
    period, hour, colon, tail = matches[0].groups()
    hour = int(old.number(hour))
    minute = int(colon) if colon else {'半': 30, '一刻': 15, '三刻': 45, '整': 0, None: 0}.get(tail)
    if minute is None: minute = int(old.number(tail.removesuffix('分')))
    if not 0 <= hour <= 23 or not 0 <= minute < 60:
        return None, ('reminder_tools.reply.clock_value.004' if codes else wording('reminder_tools.reply.clock_value.004'))
    if not period and not colon and 1 <= hour <= 12:
        return None, ('reminder_tools.reply.clock_value.005' if codes else wording('reminder_tools.reply.clock_value.005'))
    if period in ('下午', '傍晚', '晚上', '晚间', '夜里', '夜间'):
        if hour < 12: hour += 12
        elif hour == 12 and period != '下午': hour = 0
    elif period == '凌晨' and hour == 12: hour = 0
    elif period == '中午' and hour < 11: hour += 12
    return hour * 60 + minute, ''


def date_value(text, now):
    text = normalize_calendar(text)
    today = datetime.fromtimestamp(now, TZ).date()
    for word, offset in (('大后天', 3), ('后天', 2), ('明天', 1), ('今天', 0)):
        if word in text: return (today + timedelta(days=offset)).isoformat()
    iso = re.search(r'\d{4}[-/]\d{1,2}[-/]\d{1,2}', text)
    if iso: return datetime.strptime(iso[0].replace('/', '-'), '%Y-%m-%d').date().isoformat()
    match = re.search(r'(?:(\d{4})年)?([零〇一二两三四五六七八九十\d]{1,3})月([零〇一二两三四五六七八九十\d]{1,3})[日号]', text)
    if match:
        return datetime(int(match[1]) if match[1] else today.year,
            int(old.number(match[2])), int(old.number(match[3]))).date().isoformat()
    return None


def action(text, messages=None, view=None):
    """Read/write intent is decided before any model plan. No default create."""
    view = view if view is not None else {}
    try: query = compact(text)
    except CalendarInputError: return 'calendar_clarify'
    draft = live_draft(view)
    topic = view.get('topic')
    pending = view.pop('query_clarification', None)
    if pending and time.monotonic() < pending['expires']:
        if query in ('是', '是的', '对', '对的', '没错', '嗯', '我是在问提醒数量'):
            return 'count'
        if query in ('不是', '不是的', '否', '不对'):
            return 'decline_query'
    if (topic == 'reminders' and re.fullmatch(r'(?:我)?(?:现在)?(?:还)?有(?:多少|几条|几个)提成', query)):
        return 'clarify_query'
    if re.search(r'(?:提醒|闹钟)(?:的)?音量', query) or (topic == 'alarm_volume' and
            (short_setting(text) or short_restore(text) or re.search('大一点|小一点|大声一点|小声一点', query))):
        return 'volume'
    if draft and re.fullmatch(r'算了|不用了|取消设置|取消这次设置|先不设了', query): return 'discard'
    count = bool(re.search(r'(?:几|多少)(?:条|个|倒计时|提醒|闹钟)', query))
    domain = bool(re.search(r'提醒|闹钟|倒计时|定时', query))
    if count and (domain or topic == 'reminders'): return 'count'
    if re.fullmatch(r'(?:全部(?:都)?(?:删除|取消|清空)|(?:删除|取消|清空)(?:掉)?全部)', query): return 'bulk'
    if (time.monotonic() < view.get('cancel_until', 0) and
            re.fullmatch(r'(?:就)?第[一二三四1234](?:条|个)(?:提醒)?', query)):
        return 'cancel'
    if re.search(r'取消|删除|清空|停止|关掉|关闭', query) and (domain or
            re.search(r'第[一二三四1234](?:条|个)', query) or
            (topic == 'reminders' and re.fullmatch(r'(?:全部)?(?:都)?(?:取消|删除|清空)(?:全部|它|掉)?', query))):
        return 'bulk' if re.search(r'全部|所有|全都', query) else 'cancel'
    if query in ('全部', '所有', '都取消', '都删除') and topic == 'reminders': return 'bulk'
    if re.search(r'还[有剩].*(?:多久|多长|多少时间|多少秒|多少分钟|几分钟)|剩余时间', query):
        if domain or topic == 'reminders' or re.search('第[一二三四1234]条', query) or re.fullmatch(r'(?:还有|还剩)(?:多久|多少时间|多少秒|多少分钟|几分钟)', query):
            return 'remaining'
    if re.search(r'有哪些|有什么|有没有|查询|列出', query) and (domain or topic == 'reminders'): return 'list'
    if re.search(r'(?:提醒|倒计时).*(?:做什么|什么事|什么内容|什么事项|内容是什么|事项是什么)|提醒什么', query): return 'subject'
    if topic == 'reminders' and re.fullmatch(r'(?:请)?(?:继续(?:说)?|再说一遍|重复(?:一遍|一下)?|再播一遍)', query): return 'list'
    if draft and old.duration_seconds(old.normalize(text)) is not None and not re.search(r'设置|设定|创建|提醒我|叫我|每|星期|礼拜', query):
        return 'complete'
    if (draft and draft.get('need') in ('item', 'duration') and len(query) <= 24 and
            re.fullmatch(r'[\w\u4e00-\u9fff]+', query) and
            not re.search(r'什么|怎么|为何|为什么|几|多少|谢谢|你好|再见|算了|不用|取消|删除|设定|设置|每周|每天', query)):
        return 'complete'
    if re.search(r'提醒我|叫我|帮我设|设置|设定|设个|创建|启动.*倒计时|开始.*倒计时|^(?:请)?倒计时[零〇一二两三四五六七八九十百\d]', query):
        if domain or old.duration_seconds(query) is not None: return 'create'
    weekday_fragment = draft and draft['weekly'] and re.fullmatch(r'[一二三四五六日天1-7、和及至到]+', query)
    if draft and (weekday_fragment or re.search(r'每周|每星期|每礼拜|每天|周[一二三四五六日天]|星期|礼拜|今天|明天|后天|月|日|号', query) or
                  clock_value(query)[0] is not None or re.search('早上|上午|中午|下午|晚上|凌晨', query)):
        return 'complete'
    if CLOCK_PATTERN.fullmatch(query) or query in ('早上', '上午', '中午', '下午', '晚上', '凌晨'):
        return 'orphan'
    if domain or re.search(r'每周|每天|星期|礼拜|周[一二三四五六日天]', query): return 'clarify'
    return None


def intent(text, messages=None, view=None):
    # Routing must not consume a pending clarification; answer() handles it.
    copied = dict(view) if view is not None else None
    return action(text, messages, copied) is not None


def explicit_weekdays(query):
    if '每天' in query:
        return 127
    # Arabic clock digits immediately after a weekday aren't more weekdays.
    query = re.sub(r'(?<!\d)\d{1,2}(?:点|时|[:：])(?:\d{1,2}分?)?', '', query)
    match = re.search(r'(?:每周|每星期|每礼拜)([一二三四五六七日天1-7、，,和及至到\s周星期礼拜]+)', query)
    if not match:
        return None
    part = re.sub(r'[周星期礼拜\s]', '', match[1])
    def day(token):
        return int(token) - 1 if token.isdigit() else '一二三四五六七'.index(token) if token in '一二三四五六七' else 6
    tokens = re.findall(r'([一二三四五六七日天1-7])(?:[至到]([一二三四五六七日天1-7]))?', part)
    mask = 0
    for first, last in tokens:
        start, end = day(first), day(last) if last else day(first)
        # A wrap range is explicit, for example Friday through Monday.
        for offset in range((end - start) % 7 + 1): mask |= 1 << ((start + offset) % 7)
    return mask or None


def validate_status(status):
    if (not isinstance(status, dict) or type(status.get('ok')) is not bool or
            type(status.get('error')) is not int or not 0 <= status['error'] <= 5 or
            type(status.get('now')) is not int or not 1767225600 <= status['now'] < MAX_TIME or
            type(status.get('alarm_volume')) is not int or not 0 <= status['alarm_volume'] <= 100 or
            not isinstance(status.get('records'), list) or len(status['records']) > 4):
        raise ValueError('Invalid reminder receipt')
    ids, timers, schedules = set(), 0, 0
    for row in status['records']:
        if (not isinstance(row, dict) or not isinstance(row.get('id'), str) or
                not re.fullmatch('[a-f0-9]{16}', row['id']) or row['id'] in ids or
                row.get('kind') not in ('countdown', 'scheduled') or
                not isinstance(row.get('item'), str) or not 1 <= len(row['item'].encode()) <= 60 or
                not isinstance(row.get('text'), str) or not 1 <= len(row['text'].encode()) <= 120 or
                type(row.get('deadline')) is not int or not 1767225600 <= row['deadline'] < MAX_TIME or
                type(row.get('minute')) is not int or not 0 <= row['minute'] < 1440 or
                type(row.get('weekdays')) is not int or not 0 <= row['weekdays'] <= 127):
            raise ValueError('Invalid reminder record')
        ids.add(row['id'])
        timers += row['kind'] == 'countdown'
        schedules += row['kind'] == 'scheduled'
        if row['kind'] == 'countdown' and row['weekdays']:
            raise ValueError('Repeating countdown')
    if timers > 1 or schedules > 3:
        raise ValueError('Reminder capacity exceeded')


def rows(status):
    return sorted(status['records'], key=lambda row: (row['deadline'], row['id']))


def time_words(timestamp, now):
    when, today = datetime.fromtimestamp(timestamp, TZ), datetime.fromtimestamp(now, TZ).date()
    delta = (when.date() - today).days
    date = '今天' if delta == 0 else '明天' if delta == 1 else (
        (f'{when.year}年' if when.year != today.year else '') + f'{when.month}月{when.day}日')
    clock = f'{chinese_volume(when.hour)}点' + (f'{chinese_volume(when.minute)}分' if when.minute else '' if when.second else '整')
    return date + clock + (wording('reminder_tools.reply.time_words.006', p0=(chinese_volume(when.second))) if when.second else '')


def rule_words(row):
    mask = row['weekdays']
    if not mask:
        return ''
    days = '、'.join('一二三四五六日'[day] for day in range(7) if mask & (1 << day))
    prefix = '每天' if mask == 127 else f'每周{days}'
    minute = row['minute'] % 60
    return prefix + chinese_volume(row['minute'] // 60) + wording('reminder_tools.reply.rule_words.007') + (chinese_volume(minute) + wording('reminder_tools.reply.rule_words.008') if minute else wording('reminder_tools.reply.rule_words.009')) + '，'


def remaining_words(seconds):
    days, rest = divmod(seconds, 86400)
    prefix = (chinese_volume(days) if days <= 100 else str(days)) + '天' if days else ''
    return prefix + old.spoken_duration(rest) if seconds else wording('reminder_tools.reply.remaining_words.010')


def listing(status, view):
    ordered = rows(status)
    # Snapshot IDs stay attached to the spoken numbers across later changes.
    view['ids'] = [row['id'] for row in ordered]
    if not ordered:
        return wording('reminder_tools.reply.listing.011')
    return ''.join(wording('reminders.reply.list_entry', p0=chinese_volume(i), p1=row['item'], p2=rule_words(row),
                          p3='下次' if row['weekdays'] else '', p4=time_words(row['deadline'], status['now']))
                   for i, row in enumerate(ordered, 1))


def select(query, status, view):
    ordered = rows(status)
    ordinal = re.search(r'第([一二三四1234])(?:条|个)', query)
    if ordinal:
        token = ordinal[1]
        index = int(token) - 1 if token.isdigit() else '一二三四'.index(token)
        identities = view.get('ids', [])
        if index >= len(identities):
            return None
        return next((row for row in ordered if row['id'] == identities[index]), None)
    if '倒计时' in query and not re.search(r'定时|提醒或|提醒和', query):
        return next((row for row in ordered if row['kind'] == 'countdown'), None)
    if '定时' in query and '倒计时' not in query:
        schedules = [row for row in ordered if row['kind'] == 'scheduled']
        return schedules[0] if len(schedules) == 1 else None
    matched = [row for row in ordered if row['item'] in query]
    if len(matched) == 1:
        return matched[0]
    return ordered[0] if len(ordered) == 1 else None


def next_weekly(minute, mask, after):
    start = datetime.fromtimestamp(after, TZ)
    for days in range(8):
        date = start.date() + timedelta(days=days)
        candidate = datetime(date.year, date.month, date.day, minute // 60, minute % 60, tzinfo=TZ)
        if mask & (1 << date.weekday()) and candidate.timestamp() > after:
            return int(candidate.timestamp())
    raise ValueError('Empty weekly rule')


def conflict(a, b):
    if not a['weekdays'] and not b['weekdays']:
        return abs(a['deadline'] - b['deadline']) < 300
    if not a['weekdays'] or not b['weekdays']:
        once, repeat = (a, b) if not a['weekdays'] else (b, a)
        candidate = next_weekly(repeat['minute'], repeat['weekdays'], max(once['deadline'] - 300, repeat['deadline'] - 1))
        return abs(once['deadline'] - candidate) < 300
    for i in range(7):
        for j in range(7):
            if a['weekdays'] & (1 << i) and b['weekdays'] & (1 << j):
                distance = abs(i * 1440 + a['minute'] - j * 1440 - b['minute'])
                if min(distance, 10080 - distance) < 5:
                    return True
    return False


async def answer(provider, messages, execute, prepare, view):
    try: query = normalize_calendar(old.normalize(messages[-1]['content']))
    except CalendarInputError:
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.012')
    operation = action(query, messages, view)
    if operation is None:
        return wording('reminder_tools.reply.answer.013')
    if operation == 'orphan':
        return wording('reminder_tools.reply.answer.014')
    if operation == 'clarify_query':
        view['query_clarification'] = {'expires': time.monotonic() + 60}
        return wording('reminder_tools.reply.answer.015')
    if operation == 'decline_query':
        return wording('reminder_tools.reply.answer.016')
    if operation == 'discard':
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.017')
    if operation not in ('cancel', 'bulk'):
        view.pop('cancel_until', None)
    if operation == 'cancel':
        view.pop('draft', None)
    view['topic'] = 'alarm_volume' if operation == 'volume' else 'reminders'
    status = await execute(GET, {})
    validate_status(status)
    if not status['ok']:
        raise ValueError('Reminder query failed')
    # Alarm volume still uses the accepted actual-device acknowledgement path.
    if operation == 'volume':
        if short_restore(query):
            return wording('reminder_tools.reply.answer.018')
        if not re.search('调|设|静音|大一点|小一点|大声|小声', query):
            return wording('reminder_tools.reply.answer.019', p0=(chinese_volume(status['alarm_volume'])))
        values = await old.plan(provider, messages,
            wording('reminder_tools.prompt.answer.020', p0=(status['alarm_volume'])),
            {'volume': {'type': 'integer', 'minimum': 0, 'maximum': 100}}, ['volume'])
        volume = values.get('volume')
        if '静音' in query: volume = 0
        elif re.search('大一点|大声一点', query): volume = min(100, status['alarm_volume'] + 10)
        elif re.search('小一点|小声一点', query): volume = max(0, status['alarm_volume'] - 10)
        if type(volume) is not int or not 0 <= volume <= 100:
            raise ValueError('Invalid reminder volume')
        result = await execute(VOLUME, {'volume': volume})
        validate_status(result)
        if not result['ok'] or result['alarm_volume'] != volume:
            raise ValueError('Reminder volume not confirmed')
        return wording('reminder_tools.reply.answer.021', p0=(chinese_volume(volume)))
    if operation == 'count':
        total = len(status['records'])
        timers = sum(row['kind'] == 'countdown' for row in status['records'])
        return (wording('reminder_tools.reply.answer.022', p0=(chinese_volume(total)), p1=(chinese_volume(timers)), p2=(chinese_volume(total - timers)))) if total else wording('reminder_tools.reply.listing.011')
    if operation == 'bulk':
        view.pop('draft', None)
        if status['records']: view['cancel_until'] = time.monotonic() + DRAFT_SECONDS
        else: view.pop('cancel_until', None)
        return (wording('reminder_tools.reply.answer.023') + listing(status, view)
                if status['records'] else listing(status, view))
    cancel, remaining, subject = (operation == key for key in ('cancel', 'remaining', 'subject'))
    if cancel or remaining or subject:
        if subject and '定时' in query and '倒计时' not in query and not any(row['kind'] == 'scheduled' for row in status['records']):
            return wording('reminder_tools.reply.answer.024')
        if not status['records']:
            return listing(status, view)
        chosen = select(query, status, view)
        if not chosen:
            if cancel: view['cancel_until'] = time.monotonic() + DRAFT_SECONDS
            return wording('reminder_tools.reply.answer.025') + listing(status, view)
        if cancel:
            view.pop('draft', None)
            result = await execute(CANCEL, {'cache_id': chosen['id']})
            validate_status(result)
            if not result['ok']:
                return wording('reminder_tools.reply.answer.026') + listing(result, view)
            if any(row['id'] == chosen['id'] for row in result['records']):
                raise ValueError('Cancellation not confirmed')
            view.pop('cancel_until', None)
            return wording('reminder_tools.reply.answer.027', p0=(rule_words(chosen)), p1=(chosen['item']))
        if remaining:
            seconds = max(0, chosen['deadline'] - status['now'])
            return wording('reminder_tools.reply.answer.028', p0=(chosen['item']), p1=(remaining_words(seconds)))
        return wording('reminder_tools.reply.answer.029', p0=(chosen['item']))
    if operation == 'list':
        return listing(status, view)
    if operation == 'clarify':
        return wording('reminder_tools.reply.answer.030')
    if operation not in ('create', 'complete'):
        return wording('reminder_tools.reply.answer.031')
    if '工作日' in query or '调休' in query or '节假日' in query:
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.032')
    # Only an explicit pending task can provide omitted fields. Completed chat
    # pairs (including previously refused requests) are never planning input.
    draft = live_draft(view) if operation == 'complete' else None
    draft = dict(draft) if draft else {'expires': time.monotonic() + DRAFT_SECONDS, 'source': '',
        'minute': None, 'weekdays': None, 'weekly': False, 'date': None, 'seconds': None}
    source = (draft['source'] + '\n补充：' + query) if draft['source'] else query
    if len(source.encode()) > 6000:
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.033')
    draft['source'] = source
    draft['timer'] = draft.get('timer', False) or '倒计时' in query
    # Calendar/weekday evidence always wins over duration-looking ASR fragments.
    weekday_context = bool(re.search(r'每天|每周|每星期|每礼拜|(?:星期|礼拜|周)[一二三四五六七日天1-7]', source))
    if weekday_context:
        draft['seconds'] = None
    seconds_from_query = old.duration_seconds(query)
    minute, time_question = clock_value(query)
    if weekday_context and minute is None and re.search(r'每天|每周|每星期|每礼拜|星期|礼拜|周[一二三四五六日天1-7]', query) and re.search(r'\d+[.．]?\d*分', query):
        draft['minute'] = None
    if draft.get('clock_fragment') and re.fullmatch(r'\s*(?:早上|上午|中午|下午|晚上|凌晨)[。！？!?\s]*', query):
        minute, time_question = clock_value(compact(query) + draft['clock_fragment'])
        draft['minute'] = minute
    if minute is None and clock_value(query, codes=True)[1] == 'reminder_tools.reply.clock_value.005':
        draft['clock_fragment'] = CLOCK_PATTERN.search(query)[0].strip()
    clock_present = minute is not None or clock_value(query, codes=True)[1] != 'reminder_tools.reply.clock_value.002'
    explicit_duration = minute is None and '后' in query and re.search(r'小时|钟头|分钟|秒', query)
    calendar_context = bool(re.search(r'年|月|[日号]|每周|星期|礼拜|今天|明天|后天|上午|下午|晚上|早上|凌晨|中午', query))
    if not weekday_context and seconds_from_query is not None and ('倒计时' in query or explicit_duration or
                                          (not clock_present and not calendar_context)):
        draft['seconds'] = seconds_from_query
    seconds = draft['seconds']
    relative = seconds is not None
    if draft['timer'] and not weekday_context and seconds is None:
        draft['need'] = 'duration'
        view['draft'] = draft
        return wording('reminder_tools.reply.answer.034')
    if relative and seconds is not None and (not 60 <= seconds <= 86400 or seconds % 1):
        draft.update(seconds=None, timer=True, need='duration')
        view['draft'] = draft
        return old.RANGE_REPLY
    if relative and any(row['kind'] == 'countdown' for row in status['records']):
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.035')
    if not relative and sum(row['kind'] == 'scheduled' for row in status['records']) >= 3:
        view.pop('draft', None)
        return wording('reminder_tools.reply.answer.036')
    if not relative:
        # An explicitly ambiguous replacement must not inherit an older time.
        if re.search(r'点|[:：]|时', query) and re.search(r'[零〇一二两三四五六七八九十\d](?:点|[:：]|时)', query):
            draft['minute'] = minute
        elif minute is not None:
            draft['minute'] = minute
        mask = explicit_weekdays(query)
        if mask is None and draft['weekly'] and (re.search(r'周|星期|礼拜', query) or
                re.fullmatch(r'[一二三四五六日天1-7、，,和及至到\s。！？!?]+', query)):
            mask = explicit_weekdays('每周' + query)
        if mask is not None: draft['weekdays'] = mask
        draft['weekly'] = draft['weekly'] or bool(re.search(r'每周|每星期|每礼拜|每天', query))
        try:
            spoken_date = date_value(query, status['now'])
        except (ValueError, OverflowError):
            view.pop('draft', None)
            return wording('reminder_tools.reply.answer.037')
        if spoken_date: draft['date'] = spoken_date
        if draft['minute'] is None:
            view['draft'] = draft
            return time_question
        if draft['weekly'] and draft['weekdays'] is None:
            view['draft'] = draft
            return wording('reminder_tools.reply.answer.038')
        if not draft['weekly'] and not draft['date'] and re.search(r'(?:下周|本周|这周|星期|礼拜|周[一二三四五六日天])', source):
            view['draft'] = draft
            return wording('reminder_tools.reply.answer.039')
    # Retain explicit fields if a later clarification asks for a different date
    # or missing subject; never retrieve them from completed conversation pairs.
    view['draft'] = draft
    default_timer = relative and timer_without_subject(source)
    if operation == 'complete' and draft.get('need') == 'item' and compact(query) in (
            '那是', '那个', '这个', '是', '是的', '对', '好的', '嗯', '啊'):
        return wording('reminder_tools.reply.answer.040')
    # A bare "提醒我" has no subject. Ask before calling the model, even when
    # the previous turn supplied only the date and this turn supplied a clock.
    subjects = re.findall(r'(?:提醒我|叫我)([^。！？!?\n]*)', source)
    supplied_subject = operation == 'complete' and draft.get('need') == 'item' and not re.search(r'提醒我|叫我', query)
    if subjects and not any(subject.strip() for subject in subjects) and not supplied_subject and not default_timer:
        draft['need'] = 'item'
        return wording('reminder_tools.reply.answer.041')
    now = datetime.fromtimestamp(status['now'], TZ)
    instruction = (wording('reminder_tools.prompt.answer.042', p0=(now.isoformat())))
    instruction += (wording('reminder_tools.prompt.answer.043') + str(int(seconds)) + wording('reminder_tools.prompt.answer.044') if relative else
        wording('reminder_tools.prompt.answer.045', p0=(draft['minute'] // 60), p1=(draft['minute'] % 60), p2=(draft['weekdays'] or 0), p3=(draft['date'] or now.date().isoformat())))
    instruction += wording('reminder_tools.prompt.answer.046')
    properties = {'kind': {'type': 'string', 'enum': ['countdown', 'scheduled']},
        'seconds': {'type': 'integer'}, 'date': {'type': 'string'}, 'time': {'type': 'string'},
        'weekdays': {'type': 'array', 'items': {'type': 'integer', 'minimum': 1, 'maximum': 7}, 'maxItems': 7},
        'item': {'type': 'string', 'maxLength': 20}, 'text': {'type': 'string', 'maxLength': 40},
        'clarify': {'type': 'string', 'maxLength': 80}}
    planning = [dict(messages[0])] if messages[0].get('role') == 'system' else []
    planning.append({'role': 'user', 'content': source})
    planned_at = time.monotonic()
    try:
        values = ({'item': '倒计时', 'text': wording('reminder_tools.reply.answer.047'), 'clarify': ''} if default_timer else
                  await old.plan(provider, planning, instruction, properties, list(properties), max_tokens=512))
    except old.PlanValidationError:
        LOG.info('REMINDER CLARIFY: reason=invalid_model_plan; no write')
        draft['need'] = 'item'
        return wording('reminder_tools.reply.answer.048')
    LOG.info('REMINDER PLAN: elapsed_ms=%s', round((time.monotonic() - planned_at) * 1000))
    if isinstance(values.get('clarify'), str) and values['clarify']:
        draft['need'] = 'item'
        view['draft'] = draft
        return wording('reminder_tools.reply.answer.041')
    if any(not isinstance(values.get(key), str) for key in ('item', 'text')):
        draft['need'] = 'item'
        return wording('reminder_tools.reply.answer.049')
    item, text = (clean_spoken_text(values.get(key, '')) for key in ('item', 'text'))
    if not item:
        draft['need'] = 'item'
        view['draft'] = draft
        return wording('reminder_tools.reply.answer.041')
    if not 1 <= len(item.encode()) <= 60 or not 1 <= len(text.encode()) <= 120:
        draft['need'] = 'item'
        return wording('reminder_tools.reply.answer.050')
    if item not in source and not default_timer:
        draft['need'] = 'item'
        view['draft'] = draft
        return wording('reminder_tools.reply.answer.049')
    identity = secrets.token_hex(8)
    args = {'cache_id': identity, 'item': item, 'text': text}
    if relative:
        seconds = int(seconds)
        if type(seconds) is not int or not 60 <= seconds <= 86400:
            return old.RANGE_REPLY
        args['seconds'] = seconds
        name = TIMER
    else:
        mask = draft['weekdays'] if draft['weekly'] else 0
        minute = draft['minute']
        hour = minute // 60
        if mask:
            deadline = next_weekly(minute, mask, status['now'] + 59)
        else:
            try:
                selected = datetime.strptime(draft['date'], '%Y-%m-%d').date() if draft['date'] else now.date()
                deadline = int(datetime(selected.year, selected.month, selected.day, hour, minute % 60, tzinfo=TZ).timestamp())
            except (ValueError, OverflowError):
                return wording('reminder_tools.reply.answer.037')
            if deadline < status['now'] + 60:
                return wording('reminder_tools.reply.answer.051')
        if not deadline < MAX_TIME:
            return wording('reminder_tools.reply.answer.052')
        args.update(deadline=deadline, weekdays=mask, minute=minute)
        if any(conflict(args, row) for row in status['records'] if row['kind'] == 'scheduled'):
            return wording('reminder_tools.reply.answer.053')
        name = START
    view.pop('draft', None)
    result = await create_reminder(name, args, prepare, execute)
    if not result['ok']:
        return wording({1: 'reminders.reply.quota', 2: 'reminders.reply.spacing',
                        3: 'reminders.reply.not_ready'}.get(result['error'], 'reminders.reply.save_failed'))
    stored = next(row for row in result['records'] if row['id'] == identity)
    if default_timer:
        return wording('reminder_tools.reply.answer.054', p0=(old.spoken_duration(seconds)))
    return wording('reminder_tools.reply.answer.055', p0=(rule_words(stored)), p1=(time_words(stored['deadline'], result['now'])), p2=(item))
