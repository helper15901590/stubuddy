"""Read-only checks preventing a different board/profile from being flashed."""
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def verify(build_dir: Path) -> None:
    project = json.loads((build_dir / "project_description.json").read_text(encoding="utf-8"))
    config = json.loads((build_dir / "config/sdkconfig.json").read_text(encoding="utf-8"))
    assert project["target"] == "esp32s3", "Wrong chip target"
    for option in ("BOARD_TYPE_STUBUDDY_S3_N16R8", "XIAOZHI_HEADLESS_STARTUP", "SPIRAM",
                   "SPIRAM_MODE_OCT", "FLASH_NONE_ASSETS"):
        assert config.get(option) is True, f"Required config is missing: {option}"
    excluded = [name for name in project["build_components"]
                if name in {"bt", "esp_wifi", "esp_lcd", "lvgl"}
                or any(token in name for token in ("camera", "esp_video", "lvgl", "fonts", "esp-sr"))]
    assert not excluded, f"Unexpected component dependencies: {excluded}"

    entries = {}
    raw = (build_dir / "partition_table/partition-table.bin").read_bytes()
    for offset in range(0, len(raw), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", raw[offset:offset + 32])
        if magic == 0x50AA:
            entries[label.split(b"\0", 1)[0].decode()] = (address, size)
    assert entries == {"nvs": (0x9000, 0x6000), "phy_init": (0xF000, 0x1000),
                       "factory": (0x10000, 0x100000)}, "Unexpected partition layout"
    flash = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    assert {int(key, 0) for key in flash["flash_files"]} == {0, 0x8000, 0x10000}, "Flash list overlaps saved data"

    compiler = Path(project["c_compiler"])
    nm = compiler.with_name(compiler.name.replace("gcc", "nm"))
    symbols = subprocess.check_output([str(nm), "--defined-only", str(build_dir / "xiaozhi.elf")], text=True)
    assert sum(line.split()[-1] == "_Z12create_boardv" for line in symbols.splitlines() if line.split()) == 1, "Expected exactly one board factory"
    assert sum(line.split()[-1] == "app_main" for line in symbols.splitlines() if line.split()) == 1, "Expected exactly one app_main"
    assert "StubuddyAudioCodec" in symbols, "Board AudioCodec adapter is missing"
    size = (build_dir / "xiaozhi.bin").stat().st_size
    assert size < 0x100000, "Application exceeds the validation partition"
    print("PASS: stubuddy-s3-n16r8 / startup profile / ESP32-S3 N16R8")
    print("PASS: no display, camera, BLE, Wi-Fi or font components")
    print("PASS: NVS 0x9000+0x6000 preserved; application starts at 0x10000")
    print("PASS: one board factory, one app_main, real AudioCodec adapter")
    print(f"PASS: xiaozhi.bin = {size} bytes; hardware audibility remains to be checked")


if __name__ == "__main__":
    try:
        verify(ROOT / "build")
    except (AssertionError, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        print("Build the stubuddy-s3-n16r8-startup variant before flashing.", file=sys.stderr)
        raise SystemExit(1)
