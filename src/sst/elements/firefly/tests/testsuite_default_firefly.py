# -*- coding: utf-8 -*-

from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_firefly(SSTTestCase):

    def test_firefly_collective_regressions(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        output = out_dir / "firefly_collective_regression.out"
        error = out_dir / "firefly_collective_regression.err"
        self.run_sst(str(test_dir / "collective_regression.py"),
            str(output), str(error), timeout_sec=10)
        self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
        text = output.read_text(encoding="utf-8")
        self.assertEqual(1, text.count("Firefly untagged empty Request PASS"))
        self.assertEqual(1, text.count("Firefly MAX/I32/128 signature translation PASS"))
        self.assertEqual(1, text.count("Firefly RecoverableError restart handoff PASS"))
        self.assertRegex(text, r"nic\.rcvdPkts.*Sum\.u64 = 1;")
