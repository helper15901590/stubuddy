"""Generate ignored local connection settings shared by firmware and echo service."""
import argparse
import ipaddress
import json
import re
import secrets
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[2]
FIRMWARE = WORKSPACE / "xiaozhi-esp32-main/xiaozhi-esp32-main"


def select_address(output):
    candidates = json.loads(output or "[]")
    if isinstance(candidates, str):
        candidates = [candidates]
    if not isinstance(candidates, list):
        raise ValueError("Cannot identify the physical LAN interface; use --server-ip")
    addresses = sorted(set(value for value in candidates if isinstance(value, str)))
    if len(addresses) != 1:
        raise ValueError("Choose the computer LAN IPv4 with --server-ip. Candidates: " + ", ".join(addresses))
    return addresses[0]


def local_ip():
    if os.name != "nt":
        raise ValueError("Specify the computer LAN IPv4 with --server-ip")
    # A default UDP route can belong to a VPN. Use an UP physical adapter with
    # a gateway; leave ambiguous choices to the user.
    command = ("$ErrorActionPreference='Stop'; Get-NetIPConfiguration | "
               "Where-Object { $_.NetAdapter.Status -eq 'Up' -and "
               "$_.NetAdapter.HardwareInterface -and $_.IPv4DefaultGateway } | "
               "ForEach-Object { $_.IPv4Address.IPAddress } | ConvertTo-Json -Compress")
    try:
        result = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
                                 "-Command", command], capture_output=True, text=True,
                                encoding="utf-8", timeout=20, check=True)
        return select_address(result.stdout)
    except (subprocess.SubprocessError, OSError) as error:
        raise ValueError("Cannot read Windows adapter information; use --server-ip") from error


def configure(address, port):
    ip = ipaddress.IPv4Address(address)
    if not ip.is_private or ip.is_loopback or ip.is_unspecified or ip.is_multicast:
        raise ValueError("Choose this computer's private LAN IPv4 address, not 127.0.0.1")
    if not 1024 <= port <= 65535:
        raise ValueError("Choose a port between 1024 and 65535")
    settings = ROOT / ".local.json"
    token = None
    if settings.exists():
        previous = json.loads(settings.read_text(encoding="utf-8"))
        if not isinstance(previous, dict):
            raise ValueError("Invalid existing local settings; preserve the file and inspect it")
        token = previous.get("token")
        if not isinstance(token, str) or not re.fullmatch(r"[a-f0-9]{64}", token):
            raise ValueError("Invalid existing local token; preserve the file and inspect it")
    if token is None:
        token = secrets.token_hex(32)
    url = f"ws://{ip}:{port}/stubuddy/echo"
    settings.write_text(json.dumps({"host": str(ip), "port": port, "token": token}, indent=2) + "\n", encoding="utf-8")
    header = FIRMWARE / "tmp/stubuddy_echo_local.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("#pragma once\nnamespace stubuddy::echo_local {\n"
                      + "inline constexpr char kUrl[] = " + json.dumps(url) + ";\n"
                      + "inline constexpr char kToken[] = " + json.dumps(token) + ";\n}\n", encoding="utf-8")
    print("Configured:", url)
    print("Local token saved to ignored settings; token is not printed.")
    print("Check that this IPv4 belongs to the router used by the device.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ip", help="Computer LAN IPv4; omit to detect a physical adapter")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    try:
        configure(args.server_ip or local_ip(), args.port)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
