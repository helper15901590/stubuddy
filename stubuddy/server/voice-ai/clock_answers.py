"""Deterministic Beijing clock answers; timestamps come from a real clock."""
from datetime import datetime, timedelta, timezone
import re
import time
from device_tools import chinese_volume

TZ = timezone(timedelta(hours=8))


def intent(query):
    text = re.sub(r'[\s，,。.!！?？]', '', query)
    text = re.sub(r'^(?:请问(?:一下)?|请告诉我(?:一下)?|告诉我(?:一下)?|你知道)', '', text)
    text = re.sub(r'(?:吗|呢|了|呀)$', '', text)
    text = text.replace('现在北京时间', '北京时间').replace('今天的日期', '今天日期')
    if re.fullmatch(r'(?:现在|此刻|当前|北京)(?:的)?时间(?:是)?(?:多少|几点|几点几分|什么)|'
                    r'(?:现在|此刻|当前|北京时间)?(?:是)?几点(?:钟)?(?:几分|多少分)|'
                    r'(?:帮我|给我)?(?:报(?:一下|下)?时|报(?:一下|下)?(?:现在的?)?时间)', text):
        return 'time', 0, '现在'
    if re.fullmatch(r'(?:现在|此刻|当前|北京时间)?(?:是)?(?:几点(?:钟|几分)?|多少点)|(?:现在|此刻|当前|北京时间)(?:是)?(?:什么时间|时间是多少)|现在的时间(?:是(?:多少|什么))?', text):
        return 'time', 0, '现在'
    match = re.fullmatch(r'(今天|明天|后天|昨天|前天|现在)?(?:是)?(几月几[号日]|几号|几日|多少号|什么日期|日期(?:是什么|是多少)?|星期几|周几|礼拜几)', text)
    if match:
        label = match[1] or '今天'
        label = '今天' if label == '现在' else label
        return ('weekday' if re.search('星期|周|礼拜', match[2]) else 'date',
                {'今天': 0, '明天': 1, '后天': 2, '昨天': -1, '前天': -2}[label], label)
    return None


def answer(request, timestamp=None):
    if timestamp is None: timestamp = time.time()
    if isinstance(timestamp, bool) or not isinstance(timestamp, (int, float)) or not 1767225600 <= timestamp < 4102444800:
        raise ValueError('Clock unavailable')
    kind, offset, label = request
    current = datetime.fromtimestamp(timestamp, TZ) + timedelta(days=offset)
    if kind == 'time':
        return f'现在是北京时间{chinese_volume(current.hour)}点' + (f'{chinese_volume(current.minute)}分。' if current.minute else '整。')
    weekday = '星期' + '一二三四五六日'[current.weekday()]
    if kind == 'weekday': return f'{label}是{weekday}。'
    year = ''.join('零一二三四五六七八九'[int(digit)] for digit in str(current.year))
    return f'{label}是{year}年{chinese_volume(current.month)}月{chinese_volume(current.day)}日，{weekday}。'
