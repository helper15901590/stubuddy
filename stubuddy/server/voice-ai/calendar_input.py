"""Conservative ASR calendar ITN normalization. Never infer an ambiguous day."""
from datetime import datetime
import re

class CalendarInputError(ValueError):
    pass

AMPM = r'(?:a\.?\s*m\.?|p\.?\s*m\.?)'
JOINED = re.compile(r'(?<!\d)(\d{4})[-/](\d{1,2})[-/](\d{2,4})[:：](\d{2})(?:\s*(' + AMPM + r')(?![A-Za-z]))?', re.I)
JOINED_DOT = re.compile(r'(?<!\d)(\d{4})[-/](\d{1,2})[-/](\d{2,4})[.．](\d{1,2})分(?!钟)(?:\s*(' + AMPM + r')(?![A-Za-z]))?', re.I)
DATE = re.compile(r'(?<!\d)(\d{4})[-/](\d{1,2})[-/](\d{1,2})(?!\d)')
CLOCK = re.compile(r'(?<!\d)(\d{1,2})[:：](\d{1,2})\s*(' + AMPM + r')(?![A-Za-z])', re.I)
DOT_CLOCK = re.compile(r'(凌晨|清晨|早上|早晨|上午|中午|下午|傍晚|晚上|晚间|夜里|夜间)\s*'
                       r'(\d{1,2})[.．](\d{1,2})分(?!钟)')


def hour24(hour, minute, period):
    if not 0 <= minute <= 59 or (period and not 1 <= hour <= 12) or (not period and not 0 <= hour <= 23):
        raise CalendarInputError('Invalid calendar clock')
    if not period: return hour
    return hour % 12 + (12 if period.lower().startswith('p') else 0)


def normalize(text):
    # Require an explicit day period and minute suffix: decimal durations such
    # as "10.3分钟后" must never become a clock time.
    def dot_clock(match):
        period, hour, minute = match.groups()
        hour24(int(hour), int(minute), None)
        return f'{period}{int(hour)}点{int(minute)}分'
    text = DOT_CLOCK.sub(dot_clock, text)
    def joined(match):
        year, month, digits, minute, period = match.groups()
        choices = []
        for split in (1, 2):
            day, hour = digits[:split], digits[split:]
            if not day or not hour or len(hour) > 2: continue
            try:
                if not 1 <= int(day) <= 31: continue
                clock = hour24(int(hour), int(minute), period)
            except (ValueError, OverflowError):
                continue
            choices.append((int(day), clock))
        if len(choices) != 1:
            raise CalendarInputError('Ambiguous joined calendar')
        day, clock = choices[0]
        try: date = datetime(int(year), int(month), day)
        except (ValueError, OverflowError) as error: raise CalendarInputError('Invalid calendar date') from error
        return f'{date.year}年{date.month}月{date.day}日 {clock:02}:{int(minute):02}'
    text = JOINED_DOT.sub(joined, text)
    text = JOINED.sub(joined, text)
    def date(match):
        try: value = datetime(*map(int, match.groups()))
        except (ValueError, OverflowError) as error: raise CalendarInputError('Invalid calendar date') from error
        return f'{value.year}年{value.month}月{value.day}日 '
    text = DATE.sub(date, text)
    if re.search(r'\d{4}[-/]\d{1,2}[-/]\d+', text):
        raise CalendarInputError('Unsupported joined calendar')
    def clock(match):
        hour, minute, period = match.groups()
        value = hour24(int(hour), int(minute), period)
        prefix = re.search(r'(上午|早上|凌晨|下午|晚上|中午)\s*$', text[:match.start()])
        if prefix and ((prefix[1] in ('上午', '早上', '凌晨') and value >= 12) or
                       (prefix[1] in ('下午', '晚上', '中午') and value < 12)):
            raise CalendarInputError('Conflicting calendar period')
        return f'{value:02}:{int(minute):02}'
    return CLOCK.sub(clock, text)
