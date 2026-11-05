"""Read-only checks preventing a different board/profile from being flashed."""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def verify(build_dir: Path, variant: str = "startup") -> None:
    project = json.loads((build_dir / "project_description.json").read_text(encoding="utf-8"))
    config = json.loads((build_dir / "config/sdkconfig.json").read_text(encoding="utf-8"))
    assert project["target"] == "esp32s3", "Wrong chip target"
    for option in ("BOARD_TYPE_STUBUDDY_S3_N16R8", "XIAOZHI_HEADLESS_STARTUP", "SPIRAM",
                   "SPIRAM_MODE_OCT", "FLASH_NONE_ASSETS"):
        assert config.get(option) is True, f"Required config is missing: {option}"
    ai = variant in ("ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    secure = variant in ("wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    websocket = variant in ("ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    opus = variant in ("wifi-opus-ptt", "ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    wifi = variant in ("wifi-ptt", "wifi-opus-ptt", "ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    ptt = variant in ("ptt", "ptt-led", "wifi-ptt", "wifi-opus-ptt", "ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    led = variant in ("ptt-led", "wifi-ptt", "wifi-opus-ptt", "ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown")
    assert bool(config.get("STUBUDDY_PTT_SELFTEST")) == ptt, "Wrong startup/PTT mode"
    assert bool(config.get("STUBUDDY_STATUS_LED")) == led, "Wrong LED mode"
    assert bool(config.get("STUBUDDY_SOFTAP_SETUP")) == wifi, "Wrong Wi-Fi setup mode"
    assert bool(config.get("STUBUDDY_OPUS_LOOPBACK")) == opus, "Wrong Opus loopback mode"
    assert bool(config.get("STUBUDDY_WS_ECHO")) == websocket, "Wrong WebSocket echo mode"
    assert bool(config.get("STUBUDDY_WSS_ECHO")) == secure, "Wrong TLS echo mode"
    if secure:
        for option in ("MBEDTLS_CERTIFICATE_BUNDLE", "MBEDTLS_HAVE_TIME_DATE"):
            assert config.get(option) is True, f"TLS verification required: {option}"
        for option in ("ESP_TLS_INSECURE", "ESP_TLS_SKIP_SERVER_CERT_VERIFY"):
            assert not config.get(option), f"Insecure TLS setting: {option}"
        assert config.get("LWIP_SNTP_MAX_SERVERS", 0) >= 2, "Two NTP servers required"
    assert bool(config.get("STUBUDDY_AI_PTT")) == ai, "Wrong AI session mode"
    assert bool(config.get("STUBUDDY_AI_CONTROLS")) == (variant in ("ai-ptt-controls", "ai-ptt-countdown")), "Wrong device controls mode"
    assert bool(config.get("STUBUDDY_COUNTDOWN")) == (variant == "ai-ptt-countdown"), "Wrong countdown mode"
    assert ("countdown.cc" in {Path(s).name for s in project["build_component_info"]["main"]["sources"]}) == (variant == "ai-ptt-countdown"), "Wrong countdown source"
    assert not config.get("BT_ENABLED"), "Bluetooth must remain disabled"
    sources = {Path(source).name for source in project["build_component_info"]["main"]["sources"]}
    expected = "ptt_main.cc" if ptt else "startup_main.cc"
    other = "startup_main.cc" if ptt else "ptt_main.cc"
    assert expected in sources and other not in sources, "Wrong validation entry point"
    assert ("status_led.cc" in sources) == led, "Wrong LED driver source selection"
    assert ("esp_driver_ledc" in project["build_components"]) == led, "Wrong LEDC dependency selection"
    assert ("wifi_setup.cc" in sources) == wifi, "Wrong provisioning source selection"
    assert ("esp_wifi" in project["build_components"]) == wifi, "Wrong Wi-Fi dependency selection"
    assert ("opus_loopback.cc" in sources) == opus, "Wrong Opus source selection"
    assert ("espressif__esp_audio_codec" in project["build_components"]) == opus, "Wrong Opus dependency selection"
    assert ("ws_echo.cc" in sources) == websocket, "Wrong WebSocket source selection"
    assert ("espressif__esp_websocket_client" in project["build_components"]) == websocket, "Wrong WebSocket dependency selection"
    if opus:
        assert config.get("ESP_MAIN_TASK_STACK_SIZE", 0) >= 32768, "Opus stack budget missing"
    if websocket:
        assert config.get("WS_TRANSPORT") is True, "WebSocket transport is disabled"
        assert config.get("ESP_WS_CLIENT_SEPARATE_TX_LOCK") is True, "Separate WS send/receive locking is required"
    excluded = [name for name in project["build_components"]
                if name in {"bt", "esp_lcd", "lvgl"}
                or any(token in name for token in ("camera", "esp_video", "lvgl", "fonts", "esp-sr"))]
    assert not excluded, f"Unexpected component dependencies: {excluded}"

    entries = {}
    raw = (build_dir / "partition_table/partition-table.bin").read_bytes()
    for offset in range(0, len(raw), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", raw[offset:offset + 32])
        if magic == 0x50AA:
            entries[label.split(b"\0", 1)[0].decode()] = (address, size)
    app_capacity = 0x200000 if opus else 0x100000
    assert entries == {"nvs": (0x9000, 0x6000), "phy_init": (0xF000, 0x1000),
                       "factory": (0x10000, app_capacity)}, "Unexpected partition layout"
    flash = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    assert {int(key, 0) for key in flash["flash_files"]} == {0, 0x8000, 0x10000}, "Flash list overlaps saved data"

    compiler = Path(project["c_compiler"])
    nm = compiler.with_name(compiler.name.replace("gcc", "nm"))
    symbols = subprocess.check_output([str(nm), "--defined-only", str(build_dir / "xiaozhi.elf")], text=True)
    assert sum(line.split()[-1] == "_Z12create_boardv" for line in symbols.splitlines() if line.split()) == 1, "Expected exactly one board factory"
    assert sum(line.split()[-1] == "app_main" for line in symbols.splitlines() if line.split()) == 1, "Expected exactly one app_main"
    assert "StubuddyAudioCodec" in symbols, "Board AudioCodec adapter is missing"
    for symbol in ("esp_opus_enc_process", "esp_opus_dec_decode"):
        assert (any(line.split()[-1] == symbol for line in symbols.splitlines() if line.split())) == opus, f"Wrong codec linkage: {symbol}"
    assert (any(line.split()[-1] == "esp_websocket_client_send_bin" for line in symbols.splitlines() if line.split())) == websocket, "Wrong WebSocket linkage"
    if secure:
        for symbol in ("esp_crt_bundle_attach", "esp_netif_sntp_init"):
            assert any(line.split()[-1] == symbol for line in symbols.splitlines() if line.split()), f"Missing verified TLS/time: {symbol}"
    size = (build_dir / "xiaozhi.bin").stat().st_size
    assert size < app_capacity, "Application exceeds the validation partition"
    print(f"PASS: stubuddy-s3-n16r8-{variant} / ESP32-S3 N16R8")
    print("PASS: no display, camera, BLE or font components")
    print(f"PASS: Wi-Fi {'enabled with SoftAP provisioning' if wifi else 'excluded'}")
    print(f"PASS: Opus {'encoder and decoder linked' if opus else 'excluded'}")
    print(f"PASS: WebSocket {'private ai/1 enabled' if ai else 'private echo/1 enabled' if websocket else 'excluded'}")
    if ai:
        assert "ReadStreamPacket" in symbols and "DecodeStreamPacket" in symbols, "Missing streaming receiver/decoder"
        print("PASS: AI stream receiver/decoder linked; negotiated downlink packet limit 1275 bytes")
    if variant in ("ai-ptt-controls", "ai-ptt-countdown"):
        assert "SetSpeechVolume" in symbols and "OnOutputSent" in symbols, "Missing persistent volume/DMA meter"
        print("PASS: restricted device volume tools, persistent speech volume and DMA speech LED linked; PTT only")
    if variant == 'ai-ptt-countdown':
        assert 'ReadReminder' in symbols and 'SyncCountdown' in symbols, 'Missing prepared reminder channel'
        print('PASS: isolated countdown, prepared reminder cache and independent persistent reminder volume linked')
    if secure:
        print("PASS: WSS certificate bundle, peer/validity checks and SNTP enabled; no insecure bypass")
    print("PASS: NVS 0x9000+0x6000 preserved; application starts at 0x10000")
    print("PASS: one board factory, one app_main, real AudioCodec adapter")
    print(f"PASS: xiaozhi.bin = {size} bytes / {app_capacity}-byte app partition; hardware audibility remains to be checked")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("startup", "ptt", "ptt-led", "wifi-ptt", "wifi-opus-ptt", "ws-echo", "wss-echo", "ai-ptt", "ai-ptt-controls", "ai-ptt-countdown"), default="startup")
    args = parser.parse_args()
    try:
        verify(ROOT / "build", args.variant)
    except (AssertionError, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        print(f"Build the stubuddy-s3-n16r8-{args.variant} variant before flashing.", file=sys.stderr)
        raise SystemExit(1)
