"""Host regression for migrated provisioning and network-gated local audio."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class StubuddyWifiTests(unittest.TestCase):
    def test_history_dns_boot_policy_and_recording_admission(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ("wifi.exe" if os.name == "nt" else "wifi")
            subprocess.run(compiler + ["-std=c++20", "-I" + str(ROOT / "main/boards/stubuddy-s3-n16r8"),
                                       str(ROOT / "scripts/tests/stubuddy_wifi_test.cc"),
                                       "-o", str(executable)], check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)

    def test_captive_setup_page_regressions(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required for the setup page tests")
        subprocess.run([node, "--test", str(ROOT / "scripts/tests/stubuddy_setup_page.test.cjs")],
                       check=True, capture_output=True, text=True, encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
