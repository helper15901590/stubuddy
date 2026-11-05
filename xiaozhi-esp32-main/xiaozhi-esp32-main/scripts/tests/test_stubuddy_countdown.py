"""Compile the actual countdown state, cache and receipt mailbox on the host."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CountdownTests(unittest.TestCase):
    def test_bounds_persistence_cache_integrity_and_held_key(self):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A host C++ compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ('countdown.exe' if os.name == 'nt' else 'countdown')
            subprocess.run(compiler + ['-std=c++20', '-I' + str(ROOT / 'main/boards/stubuddy-s3-n16r8'),
                str(ROOT / 'scripts/tests/stubuddy_countdown_test.cc'), '-o', str(executable)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)
