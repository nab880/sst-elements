"""Diagnostic cache-maintenance gate; no stock-BSP or silicon-cache claim."""
import os
import runpy

selection = os.environ.get("QUETZ_CACHE_TEST_ENABLE", "1")
if selection not in ("0", "1"):
    raise RuntimeError("QUETZ_CACHE_TEST_ENABLE must be 0 or 1")
runpy.run_path(
    os.path.join(os.path.dirname(__file__), "basic_quetz_raptor_fft_reference.py"),
    init_globals={"WINDOW_CACHE": selection == "1"},
)
