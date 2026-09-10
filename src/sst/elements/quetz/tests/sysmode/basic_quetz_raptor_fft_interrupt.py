"""Raptor FFT ISR completion on simulator-owned INTC source 30."""

import os
import runpy

runpy.run_path(
    os.path.join(os.path.dirname(__file__), "basic_quetz_raptor_fft_reference.py"),
    init_globals={"FFT_OVERLAP": True, "FFT_INTERRUPT": True},
)
