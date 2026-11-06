"""Validate, atomically install and restart managed service wording; root-only CLI."""
import argparse
import os
from pathlib import Path
import subprocess
from prompt_store import ROOT, DEFAULTS, load


def restart():
    for args in (['restart', 'stubuddy-ai.service'], ['is-active', '--quiet', 'stubuddy-ai.service']):
        if subprocess.run(['systemctl', *args], capture_output=True).returncode:
            raise RuntimeError('Service restart/check failed')


def write(path, data, uid, gid):
    temporary = path.with_suffix('.new')
    temporary.write_bytes(data)
    os.chown(temporary, uid, gid)
    os.chmod(temporary, 0o640)
    os.replace(temporary, path)


def authorize():
    if os.name != 'posix' or os.geteuid() != 0 or not (ROOT / '.stubuddy-ai').is_file():
        raise RuntimeError('Use root on the managed AI installation')
    import pwd
    return pwd.getpwnam('stubuddy-ai')


def apply(candidate):
    account = authorize()
    active = ROOT / '.prompt-overrides.json'
    backup = ROOT / '.prompt-overrides.previous.json'
    # Read once so the bytes installed are exactly those checked.
    data = candidate.read_bytes()
    if len(data) > 262144: raise ValueError('Candidate too large')
    staged = ROOT / '.prompt-overrides.validating.json'
    write(staged, data, account.pw_uid, account.pw_gid)
    _, _, revision = load(DEFAULTS, staged)
    previous = active.read_bytes() if active.exists() else b'{}\n'
    write(backup, previous, account.pw_uid, account.pw_gid)
    write(active, data, account.pw_uid, account.pw_gid)
    try: restart()
    except Exception:
        write(active, previous, account.pw_uid, account.pw_gid)
        restart()
        raise RuntimeError('Update failed; previous override restored') from None
    print('PROMPTS APPLIED: revision=' + revision + '; service active; previous override saved')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    option = parser.add_mutually_exclusive_group(required=True)
    option.add_argument('--apply', type=Path)
    option.add_argument('--rollback', action='store_true')
    args = parser.parse_args()
    try: apply(ROOT / '.prompt-overrides.previous.json' if args.rollback else args.apply)
    except Exception as error:
        raise SystemExit('PROMPTS UPDATE FAIL: ' + type(error).__name__ + '; active file preserved or rollback attempted; inspect service status') from None
