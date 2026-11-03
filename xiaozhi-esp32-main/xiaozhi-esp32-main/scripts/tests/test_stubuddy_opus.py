"""Exercise bounded Opus framing and exact PCM delay/padding removal on the host."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class StubuddyOpusTests(unittest.TestCase):
    def test_packet_bounds_tail_flush_delay_alignment_and_stale_audio_erasure(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ("opus.exe" if os.name == "nt" else "opus")
            command = compiler + ["-std=c++20", "-I" + str(ROOT / "main/boards/stubuddy-s3-n16r8"),
                                  str(ROOT / "scripts/tests/stubuddy_opus_test.cc"), "-o", str(executable)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
