"""Regression checks for the board's release identity and isolated defaults."""
import contextlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/stubuddy-s3-n16r8"
SPEC = importlib.util.spec_from_file_location("stubuddy_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class StubuddyStartupTests(unittest.TestCase):
    def test_variant_identity_is_unique_and_uses_the_correct_board_symbol(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        found = [v for v in variants if v["name"] == "stubuddy-s3-n16r8-startup"]
        self.assertEqual(len(found), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(cfg["type"], "stubuddy-s3-n16r8")
        self.assertEqual(cfg["target"], "esp32s3")
        self.assertEqual(build._resolve_board_config("stubuddy-s3-n16r8", "esp32s3", []),
                         "CONFIG_BOARD_TYPE_STUBUDDY_S3_N16R8")

    def test_ptt_variant_is_unique_and_modes_do_not_leak_between_builds(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-ptt" for v in variants), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        builds = {variant["name"]: variant for variant in cfg["builds"]}
        startup = builds["stubuddy-s3-n16r8-startup"]
        ptt = builds["stubuddy-s3-n16r8-ptt"]
        self.assertIn("CONFIG_STUBUDDY_PTT_SELFTEST=n", startup["sdkconfig_append"])
        self.assertIn("CONFIG_STUBUDDY_PTT_SELFTEST=y", ptt["sdkconfig_append"])
        self.assertEqual(startup["sdkconfig_defaults"], ptt["sdkconfig_defaults"])

    def test_led_variant_is_unique_and_older_modes_remain_dark(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-ptt-led" for v in variants), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for variant in cfg["builds"]:
            led = variant["name"] in ("stubuddy-s3-n16r8-ptt-led", "stubuddy-s3-n16r8-wifi-ptt",
                                      "stubuddy-s3-n16r8-wifi-opus-ptt", "stubuddy-s3-n16r8-ws-echo", "stubuddy-s3-n16r8-wss-echo", "stubuddy-s3-n16r8-ai-ptt")
            self.assertIn(f"CONFIG_STUBUDDY_STATUS_LED={'y' if led else 'n'}",
                          variant["sdkconfig_append"])
            if led:
                self.assertIn("CONFIG_STUBUDDY_PTT_SELFTEST=y", variant["sdkconfig_append"])

    def test_wifi_variant_preserves_offline_profiles_and_requires_led_ptt(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-wifi-ptt" for v in variants), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for variant in cfg["builds"]:
            wifi = variant["name"] in ("stubuddy-s3-n16r8-wifi-ptt",
                                       "stubuddy-s3-n16r8-wifi-opus-ptt", "stubuddy-s3-n16r8-ws-echo", "stubuddy-s3-n16r8-wss-echo", "stubuddy-s3-n16r8-ai-ptt")
            self.assertIn(f"CONFIG_STUBUDDY_SOFTAP_SETUP={'y' if wifi else 'n'}",
                          variant["sdkconfig_append"])
            if wifi:
                self.assertIn("CONFIG_STUBUDDY_STATUS_LED=y", variant["sdkconfig_append"])
                self.assertIn("CONFIG_STUBUDDY_PTT_SELFTEST=y", variant["sdkconfig_append"])

    def test_opus_variant_is_unique_and_keeps_the_pcm_fallback_independent(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-wifi-opus-ptt" for v in variants), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for variant in cfg["builds"]:
            opus = variant["name"] in ("stubuddy-s3-n16r8-wifi-opus-ptt", "stubuddy-s3-n16r8-ws-echo", "stubuddy-s3-n16r8-wss-echo", "stubuddy-s3-n16r8-ai-ptt")
            self.assertIn(f"CONFIG_STUBUDDY_OPUS_LOOPBACK={'y' if opus else 'n'}",
                          variant["sdkconfig_append"])
            self.assertEqual("main/boards/stubuddy-s3-n16r8/opus.defaults" in
                             variant["sdkconfig_defaults"], opus)

    def test_websocket_variant_is_unique_and_does_not_change_local_loopback_modes(self):
        variants = build._collect_variants(idf_version=(6, 1, 0))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-ws-echo" for v in variants), 1)
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for variant in cfg["builds"]:
            websocket = variant["name"] in ("stubuddy-s3-n16r8-ws-echo", "stubuddy-s3-n16r8-wss-echo", "stubuddy-s3-n16r8-ai-ptt")
            self.assertIn(f"CONFIG_STUBUDDY_WS_ECHO={'y' if websocket else 'n'}",
                          variant["sdkconfig_append"])
            self.assertEqual("main/boards/stubuddy-s3-n16r8/ws_echo.defaults" in
                             variant["sdkconfig_defaults"], websocket)

    def test_wss_variant_requires_verified_tls_and_does_not_change_lan_settings(self):
        cfg = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(sum(v["name"] == "stubuddy-s3-n16r8-wss-echo" for v in cfg["builds"]), 1)
        for variant in cfg["builds"]:
            secure = variant["name"] in ("stubuddy-s3-n16r8-wss-echo", "stubuddy-s3-n16r8-ai-ptt")
            self.assertIn(f"CONFIG_STUBUDDY_WSS_ECHO={'y' if secure else 'n'}", variant["sdkconfig_append"])
            self.assertEqual("main/boards/stubuddy-s3-n16r8/wss_echo.defaults" in variant["sdkconfig_defaults"], secure)
        defaults = (BOARD / "wss_echo.defaults").read_text()
        self.assertIn("CONFIG_MBEDTLS_HAVE_TIME_DATE=y", defaults)
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y", defaults)
        self.assertIn("CONFIG_ESP_TLS_INSECURE=n", defaults)
        self.assertIn("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=n", defaults)

    def test_custom_defaults_do_not_load_the_display_project_defaults(self):
        before = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as directory, contextlib.chdir(directory):
                Path("sdkconfig.defaults").write_text("CONFIG_LVGL=y\n", encoding="utf-8")
                Path("startup.defaults").write_text("CONFIG_SPIRAM=y\n", encoding="utf-8")
                with mock.patch.object(build, "_run_idf") as run:
                    build._configure_build("esp32s3", [], "stubuddy-s3-n16r8-startup", False,
                                           sdkconfig_defaults=["startup.defaults"])
                arguments = run.call_args.args
                self.assertIn("-DSDKCONFIG_DEFAULTS=startup.defaults;build/xiaozhi-build.sdkconfig.defaults",
                              arguments)
        finally:
            os.chdir(before)

    def test_invalid_defaults_preserve_existing_configuration(self):
        before = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as directory, contextlib.chdir(directory):
                Path("sdkconfig").write_text("CONFIG_VALID=y\n", encoding="utf-8")
                for invalid in ([], "wrong-type", ["missing.defaults"]):
                    with self.assertRaises(ValueError):
                        build._configure_build("esp32s3", [], "test", False,
                                               sdkconfig_defaults=invalid)
                self.assertEqual(Path("sdkconfig").read_text(encoding="utf-8"), "CONFIG_VALID=y\n")
                self.assertFalse(Path("sdkconfig.old").exists())
        finally:
            os.chdir(before)

    def test_partition_layout_preserves_accepted_nvs_and_factory_offsets(self):
        for filename, app_size in (("startup-partitions.csv", 0x100000),
                                   ("opus-partitions.csv", 0x200000)):
            with self.subTest(filename=filename):
                entries = {}
                for line in (BOARD / filename).read_text().splitlines():
                    if line.strip() and not line.startswith("#"):
                        fields = [item.strip() for item in line.split(",")]
                        entries[fields[0]] = (int(fields[3], 0), int(fields[4], 0))
                self.assertEqual(entries["nvs"], (0x9000, 0x6000))
                self.assertEqual(entries["phy_init"], (0xF000, 0x1000))
                self.assertEqual(entries["factory"], (0x10000, app_size))


if __name__ == "__main__":
    unittest.main()
