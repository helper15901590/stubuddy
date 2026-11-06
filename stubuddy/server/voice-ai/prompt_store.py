"""Versioned wording defaults with a separate, validated operations override."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import string

ROOT = Path(__file__).resolve().parent
DEFAULTS = ROOT / 'prompts/defaults.json'
OVERRIDES = ROOT / '.prompt-overrides.json'
_snapshot = None


class PromptError(ValueError):
    pass


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise PromptError('Duplicate key')
        result[key] = value
    return result


def read_json(path):
    if path.stat().st_size > 262144: raise PromptError('File too large')
    try: return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError): raise PromptError('Invalid UTF-8 JSON') from None


def slots(text):
    try:
        values = [(name, spec, conversion) for _, name, spec, conversion in string.Formatter().parse(text) if name is not None]
    except ValueError: raise PromptError('Invalid template braces') from None
    if any(not name.startswith('p') or not name[1:].isdigit() or '{' in spec or '}' in spec for name, spec, _ in values):
        raise PromptError('Invalid placeholder')
    return Counter(values)


def load(defaults=DEFAULTS, overrides=OVERRIDES):
    catalog = read_json(Path(defaults))
    patch = read_json(Path(overrides)) if Path(overrides).exists() else {}
    if not isinstance(catalog, dict) or not isinstance(patch, dict): raise PromptError('Expected JSON object')
    if set(patch) - set(catalog): raise PromptError('Unknown override key')
    merged = {}
    for key, record in catalog.items():
        text = patch.get(key, record['text'])
        if not isinstance(text, str) or not text.strip() or len(text) > record['max_chars']:
            raise PromptError('Empty or oversized entry: ' + key)
        if len(text.encode('utf-8')) > record.get('max_bytes', 16000): raise PromptError('Entry byte limit: ' + key)
        if any(ord(c) < 32 and c not in '\n\r\t' for c in text): raise PromptError('Control character: ' + key)
        if slots(text) != slots(record['text']): raise PromptError('Placeholder mismatch: ' + key)
        merged[key] = text
    digest = hashlib.sha256(json.dumps(merged, sort_keys=True, ensure_ascii=False).encode()).hexdigest()[:12]
    return merged, frozenset(patch), digest


def snapshot():
    global _snapshot
    if _snapshot is None: _snapshot = load()
    return _snapshot


def wording(key, **values):
    return snapshot()[0][key].format(**values)


def overridden(key):
    return key in snapshot()[1]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--overrides', type=Path, default=OVERRIDES)
    parser.add_argument('--defaults', type=Path, default=DEFAULTS)
    args = parser.parse_args()
    try:
        entries, patch, revision = load(args.defaults, args.overrides)
        print(f'PROMPTS PASS: entries={len(entries)} overrides={len(patch)} revision={revision}; restart required to load changes')
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit('PROMPTS FAIL: ' + (str(error) if isinstance(error, PromptError) else type(error).__name__)) from None
