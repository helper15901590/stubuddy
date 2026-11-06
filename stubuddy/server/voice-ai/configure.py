"""Prepare a separate private AI profile. Never print or accept API keys on the command line."""
import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import tarfile

ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT.parents[2] / "xiaozhi-esp32-main/xiaozhi-esp32-main"
FILES = ("prompt_admin.py", "prompt_store.py", "prompts/defaults.json", "voice_server.py", "history.py", "audio.py", "providers.py", "device_tools.py", "capabilities.py", "clock_answers.py", "calendar_input.py", "countdown_tools.py", "reminder_tools.py", "reminder_smoke.py", "requirements.txt", "install_ecs.py", "smoke_test.py", "countdown_smoke.py")


def configure(address, *, set_key=False, region=None, workspace_id=None, tts_mode=None,
              tts_model=None, tts_voice=None, clone_voice_id=None, clone_target_model=None):
    ip = ipaddress.IPv4Address(address)
    if not ip.is_global or ip.is_multicast:
        raise ValueError("Use the ECS public IPv4")
    private = ROOT / ".private.json"
    previous = json.loads(private.read_text(encoding="utf-8")) if private.exists() else {}
    if not isinstance(previous, dict):
        raise ValueError("Invalid private settings; preserve the file")
    token = previous.get("device_token", secrets.token_hex(32))
    if not isinstance(token, str) or not re.fullmatch(r"[a-f0-9]{64}", token):
        raise ValueError("Invalid existing device token; preserve the file")
    region = region or previous.get("region", "beijing")
    if region not in ("beijing", "singapore"):
        raise ValueError("Region must be beijing or singapore")
    workspace_id = workspace_id if workspace_id is not None else previous.get("workspace_id", "")
    if workspace_id and not re.fullmatch(r"[A-Za-z0-9-]{1,80}", workspace_id):
        raise ValueError("Invalid Workspace ID")
    if workspace_id:
        location = "cn-beijing" if region == "beijing" else "ap-southeast-1"
        host = f"{workspace_id}.{location}.maas.aliyuncs.com"
    else:
        host = "dashscope.aliyuncs.com" if region == "beijing" else "dashscope-intl.aliyuncs.com"
    settings = {"server_ip": str(ip), "device_token": token, "api_key": "",
                "asr_model": "qwen3-asr-flash", "llm_model": "qwen-plus",
                "tts_model": "cosyvoice-v2", "tts_mode": "builtin",
                "tts_voice": "longxiaochun_v2", "tts_rate": 1.0,
                "clone_voice_id": "", "clone_target_model": "",
                "history_rounds": 8, "history_bytes": 64000, **previous,
                "region": region, "workspace_id": workspace_id,
                "base_url": f"https://{host}/compatible-mode/v1",
                "tts_ws_url": f"wss://{host}/api-ws/v1/inference"}
    settings["server_ip"] = str(ip)
    for name, value in (("tts_mode", tts_mode), ("tts_model", tts_model), ("tts_voice", tts_voice),
                        ("clone_voice_id", clone_voice_id), ("clone_target_model", clone_target_model)):
        if value is not None:
            settings[name] = value
    if settings["tts_mode"] not in ("builtin", "clone"):
        raise ValueError("tts_mode must be builtin or clone")
    if settings["tts_mode"] == "clone" and (not settings["clone_voice_id"] or
                                           settings["clone_target_model"] != settings["tts_model"]):
        raise ValueError("Clone voice requires matching clone target and synthesis model")
    if not re.fullmatch(r"cosyvoice-[a-z0-9.-]+", settings["tts_model"]):
        raise ValueError("Use a supported CosyVoice model")
    selected_voice = settings["clone_voice_id"] if settings["tts_mode"] == "clone" else settings["tts_voice"]
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,160}", selected_voice):
        raise ValueError("Invalid voice ID")
    if set_key:
        key = getpass.getpass("DashScope API Key (hidden, saved only in ignored private settings): ").strip()
        if not key or any(c.isspace() for c in key):
            raise ValueError("API Key must not be empty or contain whitespace")
        settings["api_key"] = key
    private.write_text(json.dumps(settings, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.chmod(private, 0o600)
    header = FIRMWARE / "tmp/stubuddy_ai_private.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("#pragma once\nnamespace stubuddy::ai_private {\n"
                      + "inline constexpr char kUrl[] = " + json.dumps(f"wss://{ip}/stubuddy/ai") + ";\n"
                      + "inline constexpr char kToken[] = " + json.dumps(token) + ";\n}\n", encoding="utf-8")
    with tarfile.open(ROOT / ".deploy.tar.gz", "w:gz") as archive:
        for name in FILES:
            archive.add(ROOT / name, arcname=name, recursive=False)
        archive.add(ROOT.parent / "ws-echo/echo_protocol.py", arcname="echo_protocol.py", recursive=False)
        archive.add(private, arcname=".private.json", recursive=False)
    os.chmod(ROOT / ".deploy.tar.gz", 0o600)
    print("AI profile and firmware header prepared; secrets are not printed.")
    print("Models: ASR/LLM/TTS; default builtin voice; eight COMPLETED historical rounds plus current.")
    print("API Key configured:", "yes" if bool(settings["api_key"]) else "no (build-only; set before deploying)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ip", default="121.41.86.40")
    parser.add_argument("--set-key", action="store_true")
    parser.add_argument("--region", choices=("beijing", "singapore"))
    parser.add_argument("--workspace-id")
    parser.add_argument("--tts-mode", choices=("builtin", "clone"))
    parser.add_argument("--tts-model")
    parser.add_argument("--tts-voice")
    parser.add_argument("--clone-voice-id")
    parser.add_argument("--clone-target-model")
    args = parser.parse_args()
    try:
        configure(args.server_ip, set_key=args.set_key, region=args.region, workspace_id=args.workspace_id,
                  tts_mode=args.tts_mode, tts_model=args.tts_model, tts_voice=args.tts_voice,
                  clone_voice_id=args.clone_voice_id, clone_target_model=args.clone_target_model)
    except (ValueError, OSError, KeyError):
        raise SystemExit("Configuration failed; inspect input or permissions without printing private settings") from None
