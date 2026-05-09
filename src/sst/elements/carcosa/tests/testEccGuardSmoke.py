"""Fast smoke test for Carcosa.EccGuard: miranda STREAM -> L1 -> EccGuard -> memCtrl."""
import os
import sst

ecc_scheme = os.getenv("ECC_SCHEME", "none")
ecc_ber    = os.getenv("ECC_BER", "0.0")
ecc_correctable_ps = os.getenv("ECC_CORRECTABLE_LATENCY_PS", "0")
ecc_due_ps         = os.getenv("ECC_DUE_LATENCY_PS", "0")
ecc_escape_ps      = os.getenv("ECC_ESCAPE_LATENCY_PS", "0")
ecc_kernel_policy  = os.getenv("ECC_KERNEL_POLICY", "")

sst.setStatisticLoadLevel(6)

cpu = sst.Component("cpu", "miranda.BaseCPU")
cpu.addParams({
    "verbose": 0,
    "clock": "2.4GHz",
    "printStats": 1,
})
gen = cpu.setSubComponent("generator", "miranda.STREAMBenchGenerator")
gen.addParams({
    "verbose": 0,
    "n": 1000,
    "operandwidth": 16,
})

l1 = sst.Component("l1cache", "memHierarchy.Cache")
l1.addParams({
    "access_latency_cycles": "2",
    "cache_frequency": "2.4 GHz",
    "replacement_policy": "lru",
    "coherence_protocol": "MESI",
    "associativity": "4",
    "cache_line_size": "64",
    "L1": "1",
    "cache_size": "32KB",
})

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_end": 4096 * 1024 * 1024 - 1,
})
backend = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
backend.addParams({
    "access_time": "100 ns",
    "mem_size": "4096MiB",
})

ecc = sst.Component("ecc_guard", "Carcosa.EccGuard")
ecc.addParams({
    "verbose":                 "true",
    "state_key":               "",
    "ecc_scheme":              ecc_scheme,
    "ber":                     ecc_ber,
    "correctable_latency_ps":  ecc_correctable_ps,
    "due_latency_ps":          ecc_due_ps,
    "escape_latency_ps":       ecc_escape_ps,
    "kernel_policy":           ecc_kernel_policy,
    "apply_on_responses_only": "true",
    "seed":                    "1",
})
ecc.enableAllStatistics()

link_cpu_l1 = sst.Link("link_cpu_l1")
link_cpu_l1.connect((cpu, "cache_link", "1000ps"), (l1, "highlink", "1000ps"))

link_l1_ecc = sst.Link("link_l1_ecc")
link_l1_ecc.connect((l1, "lowlink", "50ps"), (ecc, "highlink", "50ps"))

link_ecc_mem = sst.Link("link_ecc_mem")
link_ecc_mem.connect((ecc, "lowlink", "50ps"), (memctrl, "highlink", "50ps"))
