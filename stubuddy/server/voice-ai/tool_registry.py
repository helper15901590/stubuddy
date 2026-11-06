"""Internal device-tool contract. No network listener or model-selected device ID."""
from dataclasses import dataclass
import re


class ToolError(ValueError):
    def __init__(self, code):
        self.code = code
        super().__init__(code)


@dataclass(frozen=True)
class Tool:
    name: str
    capability: str
    fields: tuple = ()
    read_only: bool = False


TOOLS = tuple(Tool(*row) for row in (
    ('self.get_device_status', 'device_controls', (), True),
    ('self.audio_speaker.set_volume', 'device_controls', ('volume',)),
    ('self.audio_speaker.restore_volume', 'volume_restore'),
    ('self.reminders.get', 'scheduled', (), True),
    ('self.reminders.countdown', 'scheduled', ('cache_id', 'item', 'text', 'seconds')),
    ('self.reminders.start', 'scheduled', ('cache_id', 'item', 'text', 'deadline', 'weekdays', 'minute')),
    ('self.reminders.cancel', 'scheduled', ('cache_id',)),
    ('self.reminders.volume', 'scheduled', ('volume',)),
))
LEGACY = tuple(Tool(*row) for row in (
    ('self.countdown.get', 'countdown', (), True),
    ('self.countdown.start', 'countdown', ('cache_id', 'text', 'seconds')),
    ('self.countdown.cancel', 'countdown'),
    ('self.countdown.volume', 'countdown', ('volume',)),
))
REGISTRY = {tool.name: tool for tool in TOOLS + LEGACY}
LIMITS = {'volume': (0, 100), 'seconds': (60, 86400),
          'deadline': (1767225600, 4102444799), 'weekdays': (0, 127), 'minute': (0, 1439)}


def catalog(capabilities):
    """Trusted adapter discovery; compatibility tools stay private."""
    return tuple(tool for tool in TOOLS if tool.capability in capabilities)


def validate(name, values, capabilities=None):
    tool = REGISTRY.get(name)
    if tool is None:
        raise ToolError('unknown_tool')
    if capabilities is not None and tool.capability not in capabilities:
        raise ToolError('unsupported_capability')
    if type(values) is not dict or set(values) != set(tool.fields):
        raise ToolError('invalid_arguments')
    for key, value in values.items():
        if key in LIMITS:
            low, high = LIMITS[key]
            valid = type(value) is int and low <= value <= high
        elif key == 'cache_id':
            valid = isinstance(value, str) and re.fullmatch('[a-f0-9]{16}', value)
        else:
            valid = (isinstance(value, str) and bool(value.strip()) and
                     len(value.encode('utf-8')) <= (60 if key == 'item' else 120) and
                     not any(ord(char) < 32 for char in value))
        if not valid:
            raise ToolError('invalid_arguments')
    return tool


def input_schema(name):
    tool = REGISTRY.get(name)
    if tool is None:
        raise ToolError('unknown_tool')
    properties = {}
    for key in tool.fields:
        if key in LIMITS:
            low, high = LIMITS[key]
            properties[key] = {'type': 'integer', 'minimum': low, 'maximum': high}
        elif key == 'cache_id':
            properties[key] = {'type': 'string', 'pattern': '^[a-f0-9]{16}$'}
        else:
            properties[key] = {'type': 'string', 'minLength': 1,
                               'maxLength': 60 if key == 'item' else 120,
                               'x-maxUtf8Bytes': 60 if key == 'item' else 120}
    return {'type': 'object', 'properties': properties,
            'required': list(tool.fields), 'additionalProperties': False}


DESCRIPTION_KEYS = {
    'self.get_device_status': 'device_tools.prompt.global.001',
    'self.audio_speaker.set_volume': 'device_tools.prompt.global.002',
    'self.audio_speaker.restore_volume': 'device_tools.prompt.global.003',
    **{'self.reminders.' + action: 'tools.description.reminders.' + action
       for action in ('get', 'countdown', 'start', 'cancel', 'volume')},
}


def definitions(capabilities):
    """Internal discovery only. Cache IDs are not a public business API."""
    from prompt_store import wording
    return [{'name': tool.name, 'description': wording(DESCRIPTION_KEYS[tool.name]),
             'inputSchema': input_schema(tool.name), 'read_only': tool.read_only,
             'capability': tool.capability} for tool in catalog(capabilities)]
