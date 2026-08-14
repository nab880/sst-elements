#!/usr/bin/env python3
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).parents[2] / "tools"
TOOL = TOOLS / "bsp_profile.py"
SPEC = importlib.util.spec_from_file_location("bsp_profile", TOOL)
bsp_profile = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(bsp_profile)

GEN_TOOL = TOOLS / "gen_bsp_compat_blocks.py"
GEN_SPEC = importlib.util.spec_from_file_location(
    "gen_bsp_compat_blocks", GEN_TOOL
)
gen_bsp_compat_blocks = importlib.util.module_from_spec(GEN_SPEC)
assert GEN_SPEC.loader
GEN_SPEC.loader.exec_module(gen_bsp_compat_blocks)
# The generated header and the board contract live in the umbrella repo,
# reachable by walking up from this file. Resolve them for the staleness test.
_ELEM = Path(__file__).parents[2]
_OVERLAY_HEADER = _ELEM / "qemu-overlay" / "hw" / "misc" / "raptor_bsp_blocks.h"
_DTIMER_HEADER = _ELEM / "qemu-overlay" / "hw" / "misc" / "raptor_dtimer_blocks.h"
_GPIO_HEADER = _ELEM / "qemu-overlay" / "hw" / "misc" / "raptor_gpio_blocks.h"


def _find_board() -> Path | None:
    for parent in Path(__file__).resolve().parents:
        candidate = parent / "boards" / "raptor" / "board.json"
        if candidate.is_file():
            return candidate
    return None


class BspProfileToolTests(unittest.TestCase):
    def test_detects_poll_and_generates_inert_profile(self):
        # GPIOB3 int-status at 0xfc09000c is a bounded-poll target on Raptor.
        records = [{"pc": "0x40000100", "address": "0xfc09000c", "block": "gpiob3",
                    "offset": "0xc", "size": 2, "op": "read", "value": "0x0",
                    "known": False} for _ in range(4)]
        loaded = []
        for item in records:
            item = dict(item)
            item["pc_int"] = int(item["pc"], 0)
            item["address_int"] = int(item["address"], 0)
            item["value_int"] = int(item["value"], 0)
            loaded.append(item)
        report = bsp_profile.analyze(loaded, {}, poll_threshold=3)
        blocks = {"gpiob3": (0xFC090000, 0x4000)}
        profile = bsp_profile.make_profile(loaded, blocks)
        reg = profile["blocks"][0]["registers"][0]
        self.assertIn("4 reads at 0xfc09000c", report)
        self.assertEqual(profile["target"], "raptor")
        self.assertEqual(profile["blocks"][0]["name"], "gpiob3")
        self.assertEqual(reg["offset"], "0xc")
        self.assertEqual(reg["writable_mask"], "0x0")

    def test_reports_write_readback_mismatch(self):
        raw = [
            {"pc": "0x1", "address": "0xfc08c010", "block": "gpiob2",
             "offset": "0x10", "size": 2, "op": "write", "value": "0x5a", "known": False},
            {"pc": "0x2", "address": "0xfc08c010", "block": "gpiob2",
             "offset": "0x10", "size": 2, "op": "read", "value": "0x0", "known": False},
        ]
        for item in raw:
            item["pc_int"] = int(item["pc"], 0)
            item["address_int"] = int(item["address"], 0)
            item["value_int"] = int(item["value"], 0)
        self.assertIn("wrote 0x5a, read 0x0", bsp_profile.analyze(raw, {}))

    def test_blocks_loaded_from_board_contract(self):
        board = _find_board()
        if board is None:
            self.skipTest("boards/raptor/board.json not reachable")
        blocks = bsp_profile.load_blocks(board)
        # Raptor allowlist members present; legacy MCF names absent.
        self.assertIn("gpiob0", blocks)
        self.assertIn("dtim2", blocks)
        self.assertEqual(blocks["gpiob0"][0], 0xFC084000)
        self.assertNotIn("pll", blocks)
        self.assertNotIn("eport", blocks)

    def test_generated_header_is_not_stale(self):
        board = _find_board()
        if board is None or not _OVERLAY_HEADER.is_file():
            self.skipTest("board contract or generated header not reachable")
        result = subprocess.run(
            [sys.executable, str(GEN_TOOL), str(board),
             "--check-header", str(_OVERLAY_HEADER)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         msg=f"raptor_bsp_blocks.h is stale:\n{result.stderr}")

    def test_dtimer_header_is_not_stale(self):
        board = _find_board()
        if board is None or not _DTIMER_HEADER.is_file():
            self.skipTest("board contract or dtimer header not reachable")
        result = subprocess.run(
            [sys.executable, str(GEN_TOOL), str(board),
             "--check-dtimer-header", str(_DTIMER_HEADER)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         msg=f"raptor_dtimer_blocks.h is stale:\n{result.stderr}")

    def test_gpio_header_is_not_stale(self):
        board = _find_board()
        if board is None or not _GPIO_HEADER.is_file():
            self.skipTest("board contract or gpio header not reachable")
        result = subprocess.run(
            [sys.executable, str(GEN_TOOL), str(board),
             "--check-gpio-header", str(_GPIO_HEADER)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         msg=f"raptor_gpio_blocks.h is stale:\n{result.stderr}")

    def test_compat_allowlist_excludes_device_blocks(self):
        # DTIM0-3 (mcf-dtimer) and GPIOB0-3 (mcf-gpio) are owned by dedicated
        # devices, so they must NOT appear in the mcf-bsp-compat allowlist header
        # (raptor_bsp_blocks.h) or two devices would claim the same aperture.
        # They must, however, remain in bsp_profile discovery (load_blocks reads
        # all bsp_compat regardless of model).
        if not _OVERLAY_HEADER.is_file():
            self.skipTest("generated compat header not reachable")
        compat_text = _OVERLAY_HEADER.read_text(encoding="utf-8")
        for name in ("dtim0", "dtim1", "dtim2", "dtim3",
                     "gpiob0", "gpiob1", "gpiob2", "gpiob3"):
            self.assertNotIn(f'"{name}"', compat_text)
        board = _find_board()
        if board is not None:
            blocks = bsp_profile.load_blocks(board)
            self.assertIn("dtim2", blocks)   # discovery still names DTIM
            self.assertIn("gpiob2", blocks)  # ... and GPIO

    def test_generator_rejects_unknown_model(self):
        board = {"regions": [
            {"name": "bad", "base": "0x1000", "size": "0x10",
             "bsp_compat": {"block": "bad", "model": "bogus"}}]}
        with self.assertRaises(ValueError):
            gen_bsp_compat_blocks.extract_blocks(board, model=None)

    def test_generator_rejects_duplicate_across_models(self):
        board = {"regions": [
            {"name": "compat", "base": "0x1000", "size": "0x10",
             "bsp_compat": {"block": "duplicate"}},
            {"name": "timer", "base": "0x2000", "size": "0x10",
             "bsp_compat": {"block": "duplicate", "model": "dtimer"}},
        ]}
        for model in (None, "compat", "dtimer"):
            with self.subTest(model=model), self.assertRaises(ValueError):
                gen_bsp_compat_blocks.extract_blocks(board, model=model)

    def test_load_blocks_reuses_generator_validation(self):
        bad_contracts = (
            {"regions": [
                {"name": "bad", "base": "0x1000", "size": "0x10",
                 "bsp_compat": {"block": "bad", "model": "bogus"}},
            ]},
            {"regions": [
                {"name": "bad", "base": "0x1000", "size": "0x10",
                 "bsp_compat": {}},
            ]},
            {"regions": [
                {"name": "one", "base": "0x1000", "size": "0x10",
                 "bsp_compat": {"block": "same"}},
                {"name": "two", "base": "0x2000", "size": "0x10",
                 "bsp_compat": {"block": "same", "model": "gpio"}},
            ]},
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "board.json"
            for board in bad_contracts:
                with self.subTest(board=board):
                    path.write_text(__import__("json").dumps(board), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        bsp_profile.load_blocks(path)

    def test_stale_header_check_fails(self):
        board = _find_board()
        if board is None:
            self.skipTest("board contract not reachable")
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "raptor_bsp_blocks.h"
            header.write_text("stale\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(GEN_TOOL), str(board),
                 "--check-header", str(header)],
                capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("is stale", result.stderr)

    def test_write_header_round_trip(self):
        board = _find_board()
        if board is None:
            self.skipTest("board contract not reachable")
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "raptor_gpio_blocks.h"
            write = subprocess.run(
                [sys.executable, str(GEN_TOOL), str(board),
                 "--write-gpio-header", str(header)],
                capture_output=True, text=True,
            )
            check = subprocess.run(
                [sys.executable, str(GEN_TOOL), str(board),
                 "--check-gpio-header", str(header)],
                capture_output=True, text=True,
            )
        self.assertEqual(write.returncode, 0, write.stderr)
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_print_json_includes_all_device_models(self):
        board = _find_board()
        if board is None:
            self.skipTest("board contract not reachable")
        result = subprocess.run(
            [sys.executable, str(GEN_TOOL), str(board), "--print-json"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        blocks = __import__("json").loads(result.stdout)["blocks"]
        self.assertIn("fbcs", blocks)
        self.assertIn("dtim2", blocks)
        self.assertIn("gpiob2", blocks)


if __name__ == "__main__":
    unittest.main()
