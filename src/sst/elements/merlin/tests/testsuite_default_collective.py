from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


class testcase_collective(SSTTestCase):

    def test_collective_contract(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = Path(out_dir) / "collective_contract.out"
        error = Path(out_dir) / "collective_contract.err"
        self.run_sst(str(Path(test_dir) / "collective_contract.py"), str(output), str(error))
        self.assertFalse(os_test_file(str(error), "-s"))
        self.assertIn("Merlin collective contract PASS", output.read_text(encoding="utf-8"))
