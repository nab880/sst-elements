"""Native P1 instruction/data TLB, control-transition, and cached-stack test.

Reuse the one-core cached native-RAM diagnostic topology. No eDMA registers or
transfers are used by this fixture; SRAM carries the control test's stack.
"""
from pathlib import Path
import os
import runpy

topology = runpy.run_path(str(Path(__file__).with_name('basic_quetz_raptor_edma_cache.py')),
                         run_name='__main__')
topology['cpu'].addParams({'appstderr': os.environ['QUETZ_STDERR_FILE']})
