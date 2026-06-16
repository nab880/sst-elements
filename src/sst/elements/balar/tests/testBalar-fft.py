# Single-link (BalarTestCPU + merlin router) GPU-driven FFT contract test.
# Same staged radix-2 trace as testQuetz-balar-fft.py, but driven through the
# original single-interface BalarTestCPU so the two host-driver models can be
# compared on identical FFT work (A/B for the architectural demo).
import os
import sst
from utils import *
import balarBlock
import balar_test_topology

args = vars(balarTestParser.parse_args())
balarBuilder = balarBlock.Builder(args)
# FFT_DUMP=1 -> dump GPU D2H result for fft_reference.py --compare (tone check).
cpu, _ = balar_test_topology.build_testcpu_router(
    balarBuilder, args["config"], args["balar_verbosity"], args["dma_verbosity"],
    enable_memcpy_dump=(os.environ.get("FFT_DUMP") == "1")
)
cpu.addParams({
    "trace_file": args["trace"],
    "cuda_executable": args["cuda_binary"],
})

sst.setStatisticLoadLevel(args["statlevel"])
sst.enableAllStatisticsForAllComponents({"type": "sst.AccumulatorStatistic"})
sst.setStatisticOutput("sst.statOutputTXT", {"filepath": args["statfile"]})
