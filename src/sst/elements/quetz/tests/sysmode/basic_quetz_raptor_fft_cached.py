"""Stock-BSP FFT with copyback DMA buffers, CPU overlap and ISR completion."""

import os
import runpy

runpy.run_path(
    os.path.join(os.path.dirname(__file__), "basic_quetz_raptor_fft_reference.py"),
    init_globals={"WINDOW_CACHE": True, "FFT_OVERLAP": True, "FFT_INTERRUPT": True},
)
