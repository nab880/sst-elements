#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path

TOOL = Path(__file__).parents[2] / "tools" / "bsp_profile.py"
SPEC = importlib.util.spec_from_file_location("bsp_profile", TOOL)
bsp_profile = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(bsp_profile)


class BspProfileToolTests(unittest.TestCase):
    def test_detects_poll_and_generates_inert_profile(self):
        records = [{"pc": "0x40000100", "address": "0xfc090004", "block": "pll",
                    "offset": "0x4", "size": 4, "op": "read", "value": "0x0",
                    "known": False} for _ in range(4)]
        loaded = []
        for item in records:
            item = dict(item)
            item["pc_int"] = int(item["pc"], 0)
            item["address_int"] = int(item["address"], 0)
            item["value_int"] = int(item["value"], 0)
            loaded.append(item)
        report = bsp_profile.analyze(loaded, {}, poll_threshold=3)
        reg = bsp_profile.make_profile(loaded)["blocks"][0]["registers"][0]
        self.assertIn("4 reads at 0xfc090004", report)
        self.assertEqual(reg["offset"], "0x4")
        self.assertEqual(reg["writable_mask"], "0x0")

    def test_reports_write_readback_mismatch(self):
        raw = [
            {"pc": "0x1", "address": "0xfc0a4000", "block": "gpio",
             "offset": "0x0", "size": 1, "op": "write", "value": "0x5a", "known": False},
            {"pc": "0x2", "address": "0xfc0a4000", "block": "gpio",
             "offset": "0x0", "size": 1, "op": "read", "value": "0x0", "known": False},
        ]
        for item in raw:
            item["pc_int"] = int(item["pc"], 0)
            item["address_int"] = int(item["address"], 0)
            item["value_int"] = int(item["value"], 0)
        self.assertIn("wrote 0x5a, read 0x0", bsp_profile.analyze(raw, {}))


if __name__ == "__main__":
    unittest.main()
