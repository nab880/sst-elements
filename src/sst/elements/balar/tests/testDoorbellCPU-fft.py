# Doorbell-pattern GPU-driven FFT: staged radix-2 Cooley-Tukey replayed over the
# cache_link scratch + flush-before-doorbell path into balar/GPGPU-Sim.
# Each of the 1 + log2(N) kernel launches re-exercises the host<->GPU command
# traffic (scratch writes, flushes, doorbell, DMA) so the run profiles the cost
# of driving the modeled GPU, not just the FFT math.
import os
import sst
from utils import *
import balarBlock
import balar_test_topology

args = vars(balarTestParser.parse_args())
balar_builder = balarBlock.Builder(args)
# FFT_DUMP=1 dumps the GPU D2H result to cudamemcpyD2H-sim-*.data so a host tool
# (fft_reference.py --compare) can do a tolerance check on non-exact (tone) inputs.
cpu, _ = balar_test_topology.build_doorbell_topology(
    balar_builder, args["config"], mode="trace",
    balar_verbosity=args["balar_verbosity"], dma_verbosity=args["dma_verbosity"],
    enable_memcpy_dump=(os.environ.get("FFT_DUMP") == "1"))
cpu.addParams({
    "trace_file": args["trace"],
    "cuda_executable": args["cuda_binary"],
})

sst.setStatisticLoadLevel(args["statlevel"])
sst.enableAllStatisticsForAllComponents({"type": "sst.AccumulatorStatistic"})
sst.setStatisticOutput("sst.statOutputTXT", {"filepath": args["statfile"]})
