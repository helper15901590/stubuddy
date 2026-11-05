"""Isolated Ubuntu AI service; extend only the existing stubuddy TLS server block."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
TARGET = Path("/opt/stubuddy-ai")
UNIT = Path("/etc/systemd/system/stubuddy-ai.service")
MARK = "    # stubuddy-ai managed location"
FILES = ("voice_server.py", "history.py", "audio.py", "providers.py", "device_tools.py", "countdown_tools.py", "requirements.txt", "echo_protocol.py", "smoke_test.py", "countdown_smoke.py", ".private.json")


def run(*arguments):
    # Avoid verbose subprocess output that could contain request bodies/secrets.
    result = subprocess.run(arguments, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("Command failed: " + Path(arguments[0]).name)


def extend_nginx(text):
    if MARK in text:
        return text
    match = re.search(r"(?m)^(\s*)location\s+(?:=\s+)?/stubuddy/echo\s*\{", text)
    if not match:
        raise ValueError("Existing stubuddy echo location not found; existing gateway preserved")
    block = '''    # stubuddy-ai managed location
    location = /stubuddy/ai {
        proxy_pass http://127.0.0.1:8766;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_buffering off;
        proxy_read_timeout 180s;
        proxy_send_timeout 180s;
    }
'''
    return text[:match.start()] + block + text[match.start():]


def selected_settings(replace_config=False):
    staged = json.loads((ROOT / ".private.json").read_text())
    existing = TARGET / ".private.json"
    if TARGET.exists() and not (TARGET / ".stubuddy-ai").is_file():
        raise ValueError("Unowned AI target; refusing replacement")
    if existing.is_file() and not replace_config:
        settings = json.loads(existing.read_text())
        # The PC still supplies the firmware endpoint/authentication profile.
        # Never silently retain a server credential that the device cannot use.
        if any(settings.get(name) != staged.get(name) for name in ("server_ip", "device_token")):
            raise ValueError("Device connection profile differs; synchronize profiles or explicitly replace configuration")
    else:
        settings = staged
    if not settings.get("api_key"):
        raise ValueError("Selected private configuration has no DashScope API Key")
    return settings


def copy_payload(replace_config=False):
    existing = TARGET / ".private.json"
    if replace_config and existing.is_file():
        backup = TARGET / ".private.before-update.json"
        shutil.copy2(existing, backup)
        os.chmod(backup, 0o600)
    for file in FILES:
        if file == ".private.json" and (TARGET / file).is_file() and not replace_config:
            continue
        shutil.copy2(ROOT / file, TARGET / file)
    os.chmod(TARGET / ".private.json", 0o600)


def preflight(replace_config=False):
    if os.name != "posix" or os.geteuid() != 0:
        raise ValueError("Run through SSH with root/sudo")
    release = Path("/etc/os-release").read_text()
    if 'ID=ubuntu' not in release or 'VERSION_ID="24.04"' not in release:
        raise ValueError("Expected Ubuntu 24.04")
    settings = selected_settings(replace_config)
    ip = str(ipaddress.IPv4Address(settings["server_ip"]))
    # Checkpoint 3.6.2 owns conf.d/stubuddy-echo.conf, not sites-enabled/default.
    configs = [Path("/etc/nginx/conf.d/stubuddy-echo.conf")]
    configs = [path for path in configs if path.is_file() and
               path.read_text().startswith("# Managed by stubuddy checkpoint 3.6.2")]
    candidates = list(dict.fromkeys(path for path in configs if "/stubuddy/echo" in path.read_text() and
                                   re.search(r"server_name\s+" + re.escape(ip) + r"\s*;", path.read_text())))
    if len(candidates) != 1:
        raise ValueError("Expected exactly one existing stubuddy IP gateway; preserve current configuration")
    nginx = candidates[0]
    extend_nginx(nginx.read_text())
    for file in FILES:
        if not (ROOT / file).is_file():
            raise ValueError("Incomplete deployment package")
    if TARGET.exists() and not (TARGET / ".stubuddy-ai").is_file():
        raise ValueError("Unowned AI target; refusing replacement")
    if UNIT.exists() and "# stubuddy-ai managed" not in UNIT.read_text():
        raise ValueError("Unowned service unit")
    with socket.socket() as check:
        if check.connect_ex(("127.0.0.1", 8766)) == 0:
            if not UNIT.is_file() or subprocess.run(["systemctl", "is-active", "--quiet", "stubuddy-ai"]).returncode:
                raise ValueError("Port 8766 is occupied by another service")
    run("nginx", "-t")
    return nginx


def install(nginx, replace_config=False):
    run("apt-get", "update")
    run("apt-get", "install", "-y", "python3-venv", "libopus0")
    if subprocess.run(["id", "-u", "stubuddy-ai"], capture_output=True).returncode:
        run("useradd", "--system", "--user-group", "--no-create-home", "--shell", "/usr/sbin/nologin", "stubuddy-ai")
    TARGET.mkdir(mode=0o750, parents=True, exist_ok=True)
    (TARGET / ".stubuddy-ai").touch()
    copy_payload(replace_config)
    run("python3", "-m", "venv", str(TARGET / ".venv"))
    run(str(TARGET / ".venv/bin/pip"), "install", "--disable-pip-version-check", "-r", str(TARGET / "requirements.txt"))
    run("chown", "-R", "stubuddy-ai:stubuddy-ai", str(TARGET))
    UNIT.write_text('''# stubuddy-ai managed
[Unit]
Description=stubuddy DashScope PTT voice assistant
After=network-online.target
Wants=network-online.target

[Service]
User=stubuddy-ai
Group=stubuddy-ai
WorkingDirectory=/opt/stubuddy-ai
ExecStart=/opt/stubuddy-ai/.venv/bin/python /opt/stubuddy-ai/voice_server.py
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
Environment=PYTHONDONTWRITEBYTECODE=1
MemoryMax=384M
TasksMax=32

[Install]
WantedBy=multi-user.target
''')
    old = nginx.read_text()
    updated = extend_nginx(old)
    if updated != old:
        backup = Path("/var/backups/stubuddy-ai")
        backup.mkdir(mode=0o700, parents=True, exist_ok=True)
        shutil.copy2(nginx, backup / "nginx.before-ai.conf")
        nginx.write_text(updated)
        try:
            run("nginx", "-t")
        except Exception:
            nginx.write_text(old)
            raise
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "stubuddy-ai.service")
    run("systemctl", "restart", "stubuddy-ai.service")
    run("systemctl", "is-active", "--quiet", "stubuddy-ai.service")
    for _ in range(50):
        with socket.socket() as check:
            check.settimeout(0.2)
            if check.connect_ex(("127.0.0.1", 8766)) == 0:
                break
        time.sleep(0.2)
    else:
        raise RuntimeError("AI service did not open loopback port; check stubuddy-ai journal")
    run("systemctl", "reload", "nginx")
    print("INSTALL PASS: isolated AI service, loopback 8766, existing TLS gateway retained")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--replace-config", action="store_true",
                        help="Explicitly replace existing server settings with the uploaded private settings")
    args = parser.parse_args()
    try:
        nginx = preflight(args.replace_config)
        print("CHECK PASS: Ubuntu 24.04, owned gateway and loopback port")
        if args.install:
            install(nginx, args.replace_config)
    except Exception as error:
        raise SystemExit("STOP: " + str(error)) from None
