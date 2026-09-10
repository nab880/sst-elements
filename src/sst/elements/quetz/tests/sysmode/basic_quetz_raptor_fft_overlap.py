"""Raptor FFT overlap with CPU checkpoints and a fixed functional latency."""

import os
import runpy

runpy.run_path(
    os.path.join(os.path.dirname(__file__), "basic_quetz_raptor_fft_reference.py"),
    init_globals={"FFT_OVERLAP": True},
)
