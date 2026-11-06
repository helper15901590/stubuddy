"""Compile actual scheduler and NVS adapter with an in-memory NVS/time double."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScheduledTests(unittest.TestCase):
    def test_real_schedule_rules_persistence_migration_claim_and_cache_bank(self):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not shutil.which(compiler[0]): self.skipTest('Host C++ compiler required')
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'esp_err.h').write_text('''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, ESP_ERR_INVALID_STATE=2;
''')
            (tmp / 'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n')
            (tmp / 'nvs.h').write_text('''#pragma once
#include <cstddef>
#include "esp_err.h"
using nvs_handle_t = int;
constexpr int NVS_READWRITE=1;
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
''')
            executable = tmp / ('scheduled.exe' if os.name == 'nt' else 'scheduled')
            command = compiler + ['-std=c++20', '-DCONFIG_STUBUDDY_COUNTDOWN=1', '-DCONFIG_STUBUDDY_SCHEDULED=1',
                '-I' + str(tmp), '-I' + str(ROOT / 'main/boards/stubuddy-s3-n16r8'),
                str(ROOT / 'scripts/tests/stubuddy_scheduled_test.cc'),
                str(ROOT / 'main/boards/stubuddy-s3-n16r8/scheduled_reminders.cc'), '-o', str(executable)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
