"""Create an independent, ignored ECS/WSS profile without changing the LAN profile."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import tarfile

ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT.parents[2] / "xiaozhi-esp32-main/xiaozhi-esp32-main"
PACKAGE_FILES = ("echo_server.py", "echo_protocol.py", "requirements.txt", "install_ecs.py")


def configure(address, email=None):
    ip = ipaddress.IPv4Address(address)
    if not ip.is_global or ip.is_multicast:
        raise ValueError("Choose the ECS public IPv4 address")
    settings = ROOT / ".cloud.json"
    token = secrets.token_hex(32)
    if settings.exists():
        previous = json.loads(settings.read_text(encoding="utf-8"))
        if not isinstance(previous, dict) or not isinstance(previous.get("token"), str):
            raise ValueError("Invalid existing cloud settings; preserve the file")
        token = previous["token"]
        if not re.fullmatch(r"[a-f0-9]{64}", token):
            raise ValueError("Invalid existing cloud token; preserve the file")
        if email is None:
            email = previous.get("email")
    if email is not None and (not isinstance(email, str) or not re.fullmatch(r"[^\s@]+@[^\s@]+\.[^\s@]+", email)):
        raise ValueError("Provide a valid ACME account contact email")
    # Validate all package inputs before changing any local settings.
    for name in PACKAGE_FILES:
        if not (ROOT / name).is_file():
            raise ValueError("Missing deployment source: " + name)
    url = f"wss://{ip}/stubuddy/echo"
    data = {"host": str(ip), "port": 8765, "url": url, "token": token}
    if email is not None:
        data["email"] = email
    settings.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.chmod(settings, 0o600)
    header = FIRMWARE / "tmp/stubuddy_echo_cloud.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("#pragma once\nnamespace stubuddy::echo_cloud {\n"
                      + "inline constexpr char kUrl[] = " + json.dumps(url) + ";\n"
                      + "inline constexpr char kToken[] = " + json.dumps(token) + ";\n}\n", encoding="utf-8")
    package = ROOT / ".cloud-deploy.tar.gz"
    # Only this explicit allowlist enters the archive; no LAN token or build files.
    with tarfile.open(package, "w:gz") as archive:
        for name in PACKAGE_FILES:
            archive.add(ROOT / name, arcname=name, recursive=False)
        archive.add(settings, arcname=".cloud.json", recursive=False)
    os.chmod(package, 0o600)
    print("Configured:", url)
    print("Cloud token preserved/generated independently; token is not printed.")
    print("Private deployment archive: stubuddy/server/ws-echo/.cloud-deploy.tar.gz")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ip", required=True, help="ECS public IPv4")
    parser.add_argument("--email", help="ACME account contact, kept in ignored cloud settings")
    args = parser.parse_args()
    try:
        configure(args.server_ip, args.email)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
