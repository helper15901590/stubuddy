"""Exercise bounded upload admission under sender stalls and repeated cancellation."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class StubuddyUplinkTests(unittest.TestCase):
    def test_stalled_sender_control_reserve_and_cancelled_credit_recovery(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ("uplink.exe" if os.name == "nt" else "uplink")
            command = compiler + ["-std=c++20", "-I" + str(ROOT / "main/boards/stubuddy-s3-n16r8"),
                                  str(ROOT / "scripts/tests/stubuddy_uplink_test.cc"), "-o", str(executable)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
