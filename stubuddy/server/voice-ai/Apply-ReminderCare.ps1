# Apply the reviewed public reminder style without replacing other overrides.
$ErrorActionPreference = 'Stop'
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'prompts/defaults.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$key = 'reminder_tools.prompt.answer.046'
$style = $catalog.$key.text
if ([string]::IsNullOrWhiteSpace($style)) { throw 'Missing reminder style.' }
$style64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($style))
$script = @'
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
root = Path('/opt/stubuddy-ai')
os.chdir(root)
sys.path.insert(0, str(root))
from prompt_store import DEFAULTS, load, read_json
active = root / '.prompt-overrides.json'
load(DEFAULTS, active)
patch = read_json(active) if active.exists() else {}
patch['reminder_tools.prompt.answer.046'] = base64.b64decode('STYLE_BASE64').decode('utf-8')
candidate = root / '.prompt-overrides.candidate.json'
os.umask(0o077)
candidate.write_text(json.dumps(patch, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
os.chmod(candidate, 0o600)
load(DEFAULTS, candidate)
subprocess.run([sys.executable, str(root / 'prompt_admin.py'), '--apply', str(candidate)], check=True)
'@
$script = $script.Replace('STYLE_BASE64', $style64)
$payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($script))
$bootstrap = "import base64; exec(base64.b64decode('$payload'))"
$bootstrap | ssh -T -p 22 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python -
if ($LASTEXITCODE -ne 0) { throw 'Reminder style update failed; retain error output.' }
