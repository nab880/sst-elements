"""Phase2: stub_cpu/gpu + delay agents; VLA_BASELINE_{CPU,GPU}_PS (18 ps values), VLA_SCALE_FACTOR. Build stubs: cd to this directory and run `make` (uses zig+musl by default; see Makefile for overrides)."""
import os
import sst

mh_debug_level = int(os.getenv("MH_DEBUG_LEVEL", "0"))
mh_debug = int(os.getenv("MH_DEBUG", "0"))
checkpointDir = ""
checkpoint = ""
pythonDebug = False

vanadis_isa = os.getenv("VANADIS_ISA", "RISCV64")
loader_mode = os.getenv("VANADIS_LOADER_MODE", "0")
lib = "vanadis"

cpu_exe = os.getenv("VLA_CPU_EXE", "./stub_cpu")
gpu_exe = os.getenv("VLA_GPU_EXE", "./stub_gpu")

physMemSize = "4GiB"
tlbType = "simpleTLB"
mmuType = "simpleMMU"

sst.setProgramOption("timebase", "1ps")
# Default "0 ns" runs until a component ends the simulation (e.g. delay-agent
# Hali "exit" after max_cycles). VLA_SST_STOP_AT overrides with a wall-clock
# cap; run_ecc_sweep.sh clears it so sweeps are not truncated mid-FSM.
_vla_sst_stop = os.getenv("VLA_SST_STOP_AT", "").strip()
if _vla_sst_stop and _vla_sst_stop.lower() not in ("0", "0 ns", "none"):
    sst.setProgramOption("stop-at", _vla_sst_stop)
else:
    sst.setProgramOption("stop-at", "0 ns")
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput("sst.statOutputConsole")

verbosity = int(os.getenv("VANADIS_VERBOSE", 0))
os_verbosity = os.getenv("VANADIS_OS_VERBOSE", str(verbosity))
pipe_trace_file = os.getenv("VANADIS_PIPE_TRACE", "")

cpu_clock = os.getenv("VANADIS_CPU_CLOCK", "2.3GHz")

cpuCoreParams = {
    "clock": cpu_clock, "verbose": verbosity, "hardware_threads": 1,
    "physical_fp_registers": 168, "physical_integer_registers": 180,
    "integer_arith_cycles": 2, "integer_arith_units": 2,
    "fp_arith_cycles": 8, "fp_arith_units": 2,
    "branch_unit_cycles": 2,
    "print_int_reg": False, "print_fp_reg": False,
    "pipeline_trace_file": pipe_trace_file,
    "reorder_slots": 64,
    "decodes_per_cycle": 4, "issues_per_cycle": 4, "retires_per_cycle": 4,
    "pause_when_retire_address": 0,
    "start_verbose_when_issue_address": "0",
    "stop_verbose_when_retire_address": "0",
    "print_rob": False,
    "checkpointDir": checkpointDir, "checkpoint": checkpoint,
}
cpuLsqParams = {
    "verbose": verbosity, "address_mask": 0xFFFFFFFF,
    "max_stores": 8, "max_loads": 16,
}

gpu_clock = os.getenv("VANADIS_GPU_CLOCK", "1.5GHz")

gpuCoreParams = {
    "clock": gpu_clock, "verbose": verbosity, "hardware_threads": 1,
    "physical_fp_registers": 168, "physical_integer_registers": 180,
    "integer_arith_cycles": 2, "integer_arith_units": 2,
    "fp_arith_cycles": 2, "fp_arith_units": 8,
    "branch_unit_cycles": 2,
    "print_int_reg": False, "print_fp_reg": False,
    "pipeline_trace_file": pipe_trace_file,
    "reorder_slots": 128,
    "decodes_per_cycle": 8, "issues_per_cycle": 8, "retires_per_cycle": 8,
    "pause_when_retire_address": 0,
    "start_verbose_when_issue_address": "0",
    "stop_verbose_when_retire_address": "0",
    "print_rob": False,
    "checkpointDir": checkpointDir, "checkpoint": checkpoint,
}
gpuLsqParams = {
    "verbose": verbosity, "address_mask": 0xFFFFFFFF,
    "max_stores": 16, "max_loads": 32,
}

vla_num_vit_layers  = os.getenv("VLA_NUM_VIT_LAYERS", "2")
vla_num_llm_layers  = os.getenv("VLA_NUM_LLM_LAYERS", "2")
# Pipeline cycles (actuations) before the delay agent signals exit. Prefer
# VLA_MAX_CYCLES; run_all_ecc.sh sets VLA_PHASE2_MAX_CYCLES for headline/FAST.
vla_max_cycles      = os.getenv("VLA_MAX_CYCLES") or os.getenv("VLA_PHASE2_MAX_CYCLES") or "1"
vla_initial_seq_len   = os.getenv("VLA_INITIAL_SEQ_LEN", "8")
vla_max_seq_len       = os.getenv("VLA_MAX_SEQ_LEN", "64")
vla_num_action_tokens = os.getenv("VLA_NUM_ACTION_TOKENS", "1")
vla_scale_factor      = os.getenv("VLA_SCALE_FACTOR", "1.0")
# FSM RNG: drives the decode early-exit Bernoulli. Falls back to ECC_SEED so
# the sweep script's `seed` knob varies the FSM trace as well as the fault
# draws (otherwise the FSM is deterministic across seeds and the only
# variation is which lines get bit-flipped). Set VLA_DECODE_EXIT_PROB > 0
# for the FSM seed to actually do anything; at the default 0.0 the FSM is
# fully deterministic regardless of seed.
vla_rng_seed          = os.getenv("VLA_RNG_SEED", os.getenv("ECC_SEED", "12345"))
vla_decode_exit_prob  = os.getenv("VLA_DECODE_EXIT_PROB", "0.0")

# ECC pressure-point knobs (defaults make EccGuard a transparent pass-through).
vla_state_key             = os.getenv("VLA_STATE_KEY", "cpu0_vla")
ecc_scheme                = os.getenv("ECC_SCHEME", "none")
ecc_ber                   = os.getenv("ECC_BER", "0.0")
ecc_correctable_latency_ps = os.getenv("ECC_CORRECTABLE_LATENCY_PS", "0")
ecc_due_latency_ps        = os.getenv("ECC_DUE_LATENCY_PS", "0")
ecc_escape_latency_ps     = os.getenv("ECC_ESCAPE_LATENCY_PS", "0")
ecc_kernel_policy         = os.getenv("ECC_KERNEL_POLICY", "")
ecc_seed                  = os.getenv("ECC_SEED", "0")
ecc_apply_on_responses_only = os.getenv("ECC_RESPONSES_ONLY", "true")
# Phase 2/3 ECC knobs.
ecc_fault_model           = os.getenv("ECC_FAULT_MODEL", "poisson")
ecc_fault_mode_weights    = os.getenv("ECC_FAULT_MODE_WEIGHTS", "")
ecc_fault_event_rate      = os.getenv("ECC_FAULT_EVENT_RATE", "0.0")
# Campaign-mode knobs (only consulted when ecc_fault_model == "campaign";
# inert for poisson / jedec_mix runs, so existing sweeps are unaffected).
ecc_campaign_target_kernel = os.getenv("ECC_CAMPAIGN_TARGET_KERNEL", "any")
ecc_campaign_mode          = os.getenv("ECC_CAMPAIGN_MODE",          "row")
ecc_campaign_event_budget  = os.getenv("ECC_CAMPAIGN_EVENT_BUDGET",  "0")
ecc_campaign_event_rate    = os.getenv("ECC_CAMPAIGN_EVENT_RATE",    "0.0")
ecc_campaign_max_per_entry = os.getenv("ECC_CAMPAIGN_MAX_PER_KERNEL_ENTRY", "0")
ecc_campaign_errors_fixed  = os.getenv("ECC_CAMPAIGN_ERRORS_FIXED",  "0")
ecc_campaign_force_multi_chip = os.getenv("ECC_CAMPAIGN_FORCE_MULTI_CHIP", "0")
ecc_addr_filter_region     = os.getenv("ECC_ADDR_FILTER_REGION", "")
ecc_addr_filter_len        = os.getenv("ECC_ADDR_FILTER_LEN", "0")
critical_watcher_enabled   = os.getenv("CRITICAL_ACTION_WATCHER", "1") not in ("0", "false", "False", "")
critical_watcher_region    = os.getenv("CRITICAL_WATCHER_REGION", "action_queue")
critical_watcher_len       = os.getenv("CRITICAL_WATCHER_LEN", "64")
ecc_payload_dtype         = os.getenv("ECC_PAYLOAD_DTYPE", "bytes")
ecc_due_action            = os.getenv("ECC_DUE_ACTION", "latency_only")
ecc_fit_per_mbit_per_hour = os.getenv("ECC_FIT_PER_MBIT_PER_HOUR", "0.0")
ecc_dram_capacity_mb      = os.getenv("ECC_DRAM_CAPACITY_MB", "1024")
ecc_sim_time_per_event_ns = os.getenv("ECC_SIM_TIME_PER_EVENT_NS", "100")
# Phase 1 region routing + Phase 4 scorer.
vla_regions               = os.getenv("VLA_REGIONS", "")
action_scorer_enabled     = os.getenv("ACTION_SCORER", "1") not in ("0", "false", "False", "")
action_scorer_golden      = os.getenv("ACTION_SCORER_GOLDEN", "")
action_scorer_emit_golden = os.getenv("ACTION_SCORER_EMIT_GOLDEN", "0") in ("1", "true", "True")
# Default true: when golden_log is set but cannot be opened or is empty, the
# scorer fatals instead of silently scoring every frame as not-argmax-changed.
# Set ACTION_SCORER_GOLDEN_REQUIRED=0 only for self-replay or smoke runs.
action_scorer_golden_required = os.getenv(
    "ACTION_SCORER_GOLDEN_REQUIRED", "1") not in ("0", "false", "False", "")

default_zeros = ",".join(["0"] * 18)

def _baseline_env(new_var, legacy_var):
    val = os.getenv(new_var)
    if val is not None:
        return val
    legacy = os.getenv(legacy_var)
    if legacy is not None:
        print(f"[deprecated] {legacy_var} is now {new_var}; values are interpreted as picoseconds.")
        return legacy
    return default_zeros

vla_baseline_cpu_ps = _baseline_env("VLA_BASELINE_CPU_PS", "VLA_BASELINE_CPU_CYCLES")
vla_baseline_gpu_ps = _baseline_env("VLA_BASELINE_GPU_PS", "VLA_BASELINE_GPU_CYCLES")

numCpus = 2
numThreads = 1

vanadis_cpu_type = lib + "." + os.getenv("VANADIS_CPU_ELEMENT_NAME", "dbg_VanadisCPU")
vanadis_decoder  = lib + ".Vanadis" + vanadis_isa + "Decoder"
vanadis_os_hdlr  = lib + ".Vanadis" + vanadis_isa + "OSHandler"
protocol = "MESI"

osParams = {
    "processDebugLevel": 0, "dbgLevel": os_verbosity, "dbgMask": 8,
    "cores": numCpus, "hardwareThreadCount": numThreads,
    "page_size": 4096, "physMemSize": physMemSize, "useMMU": True,
    "checkpointDir": checkpointDir, "checkpoint": checkpoint,
}

processList = (
    (1, {"env_count": 0, "exe": cpu_exe, "arg0": cpu_exe.split("/")[-1], "argc": 1}),
    (1, {"env_count": 0, "exe": gpu_exe, "arg0": gpu_exe.split("/")[-1], "argc": 1}),
)

osl1cacheParams = {
    "access_latency_cycles": "2", "cache_frequency": cpu_clock,
    "replacement_policy": "lru", "coherence_protocol": protocol,
    "associativity": "8", "cache_line_size": "64", "cache_size": "32 KB",
    "L1": "1", "debug": mh_debug, "debug_level": mh_debug_level,
}
mmuParams = {"debug_level": 0, "num_cores": numCpus, "num_threads": numThreads, "page_size": 4096}
memRtrParams = {
    "xbar_bw": "1GB/s", "link_bw": "1GB/s", "input_buf_size": "2KB",
    "num_ports": str(numCpus + 2), "flit_size": "72B", "output_buf_size": "2KB",
    "id": "0", "topology": "merlin.singlerouter",
}
dirCtrlParams = {
    "coherence_protocol": protocol, "entry_cache_size": "1024",
    "debug": mh_debug, "debug_level": mh_debug_level,
    "addr_range_start": "0x0", "addr_range_end": "0xFFFFFFFF",
}
dirNicParams  = {"network_bw": "25GB/s", "group": 2}
memCtrlParams = {
    "clock": cpu_clock, "backend.mem_size": physMemSize,
    "backing": "malloc", "initBacking": 1,
    "addr_range_start": 0, "addr_range_end": 0xffffffff,
    "debug_level": mh_debug_level, "debug": mh_debug,
    "checkpointDir": checkpointDir, "checkpoint": checkpoint,
}
memParams     = {"mem_size": "4GiB", "access_time": "1 ns"}
tlbParams     = {"debug_level": 0, "hit_latency": 1, "num_hardware_threads": numThreads,
                 "num_tlb_entries_per_thread": 64, "tlb_set_size": 4}
tlbWrapperParams = {"debug_level": 0}
decoderParams = {"loader_mode": loader_mode, "uop_cache_entries": 1536, "predecode_cache_entries": 4}
osHdlrParams  = {}
branchPredParams = {"branch_entries": 32}
busParams     = {"bus_frequency": cpu_clock}
l2memLinkParams = {"group": 1, "network_bw": "25GB/s"}

l1dcacheParams = {
    "access_latency_cycles": "2", "cache_frequency": cpu_clock,
    "replacement_policy": "lru", "coherence_protocol": protocol,
    "associativity": "8", "cache_line_size": "64", "cache_size": "32 KB",
    "L1": "1", "debug": mh_debug, "debug_level": mh_debug_level,
}
l1icacheParams = {
    "access_latency_cycles": "2", "cache_frequency": cpu_clock,
    "replacement_policy": "lru", "coherence_protocol": protocol,
    "associativity": "8", "cache_line_size": "64", "cache_size": "32 KB",
    "prefetcher": "cassini.NextBlockPrefetcher", "prefetcher.reach": 1,
    "L1": "1", "debug": mh_debug, "debug_level": mh_debug_level,
}
cpu_l2cacheParams = {
    "access_latency_cycles": "14", "cache_frequency": cpu_clock,
    "replacement_policy": "lru", "coherence_protocol": protocol,
    "associativity": "16", "cache_line_size": "64", "cache_size": "1MB",
    "mshr_latency_cycles": 3, "debug": mh_debug, "debug_level": mh_debug_level,
}
# Case-study ACTUATE readbacks must miss below L2 so EccGuard injects each frame.
if critical_watcher_enabled or ecc_fault_model == "campaign":
    cpu_l2cacheParams = dict(cpu_l2cacheParams, cache_size="32 KB")
gpu_l2cacheParams = {
    "access_latency_cycles": "14", "cache_frequency": gpu_clock,
    "replacement_policy": "lru", "coherence_protocol": protocol,
    "associativity": "16", "cache_line_size": "64", "cache_size": "4MB",
    "mshr_latency_cycles": 3, "debug": mh_debug, "debug_level": mh_debug_level,
}


def addParamsPrefix(prefix, params):
    return {prefix + "." + k: v for k, v in params.items()}


class CPU_Builder:
    def __init__(self):
        pass

    def build(self, prefix, cpuId, coreParams, lsqParams, l2Params,
              agentType, agentParams):
        cpu = sst.Component(prefix, vanadis_cpu_type)
        cpu.addParams(coreParams)
        cpu.addParam("core_id", cpuId)
        cpu.enableAllStatistics()

        for n in range(numThreads):
            decode = cpu.setSubComponent("decoder", vanadis_decoder, n)
            decode.addParams(decoderParams)
            decode.enableAllStatistics()
            os_hdlr = decode.setSubComponent("os_handler", vanadis_os_hdlr)
            os_hdlr.addParams(osHdlrParams)
            branch_pred = decode.setSubComponent("branch_unit",
                                                  lib + ".VanadisBasicBranchUnit")
            branch_pred.addParams(branchPredParams)
            branch_pred.enableAllStatistics()

        cpu_lsq = cpu.setSubComponent("lsq", lib + ".VanadisBasicLoadStoreQueue")
        cpu_lsq.addParams(lsqParams)
        cpu_lsq.enableAllStatistics()
        cpuDcacheIf = cpu_lsq.setSubComponent("memory_interface",
                                               "memHierarchy.standardInterface")
        cpuIcacheIf = cpu.setSubComponent("mem_interface_inst",
                                           "memHierarchy.standardInterface")

        cpu_l1dcache = sst.Component(prefix + ".l1dcache", "memHierarchy.Cache")
        cpu_l1dcache.addParams(l1dcacheParams)
        cpu_l1icache = sst.Component(prefix + ".l1icache", "memHierarchy.Cache")
        cpu_l1icache.addParams(l1icacheParams)
        cpu_l2cache = sst.Component(prefix + ".l2cache", "memHierarchy.Cache")
        cpu_l2cache.addParams(l2Params)
        l2cache_2_mem = cpu_l2cache.setSubComponent("lowlink", "memHierarchy.MemNIC")
        l2cache_2_mem.addParams(l2memLinkParams)
        cache_bus = sst.Component(prefix + ".bus", "memHierarchy.Bus")
        cache_bus.addParams(busParams)

        dtlbWrapper = sst.Component(prefix + ".dtlb", "mmu.tlb_wrapper")
        dtlbWrapper.addParams(tlbWrapperParams)
        dtlb = dtlbWrapper.setSubComponent("tlb", "mmu." + tlbType)
        dtlb.addParams(tlbParams)
        itlbWrapper = sst.Component(prefix + ".itlb", "mmu.tlb_wrapper")
        itlbWrapper.addParams(tlbWrapperParams)
        itlbWrapper.addParam("exe", True)
        itlb = itlbWrapper.setSubComponent("tlb", "mmu." + tlbType)
        itlb.addParams(tlbParams)

        hali = sst.Component(prefix + ".hali", "Carcosa.Hali")
        hali.addParams({"intercept_ranges": "0xBEEF0000,4096", "verbose": "true"})
        agent = hali.setSubComponent("interceptionAgent", agentType)
        agent.addParams(agentParams)

        link_cpu_hali = sst.Link(prefix + ".link_cpu_hali")
        link_cpu_hali.connect((cpuDcacheIf, "lowlink", "1ns"),
                              (hali, "highlink", "1ns"))
        link_cpu_hali.setNoCut()
        link_hali_dtlb = sst.Link(prefix + ".link_hali_dtlb")
        link_hali_dtlb.connect((hali, "lowlink", "1ns"),
                               (dtlbWrapper, "highlink", "1ns"))
        link_hali_dtlb.setNoCut()

        link_cpu_l1dcache_link = sst.Link(prefix + ".link_cpu_l1dcache_link")
        link_cpu_l1dcache_link.connect((dtlbWrapper, "lowlink", "1ns"),
                                       (cpu_l1dcache, "highlink", "1ns"))
        link_cpu_l1dcache_link.setNoCut()

        link_cpu_itlb_link = sst.Link(prefix + ".link_cpu_itlb_link")
        link_cpu_itlb_link.connect((cpuIcacheIf, "lowlink", "1ns"),
                                   (itlbWrapper, "highlink", "1ns"))
        link_cpu_itlb_link.setNoCut()
        link_cpu_l1icache_link = sst.Link(prefix + ".link_cpu_l1icache_link")
        link_cpu_l1icache_link.connect((itlbWrapper, "lowlink", "1ns"),
                                       (cpu_l1icache, "highlink", "1ns"))
        link_cpu_l1icache_link.setNoCut()

        link_l1dcache_l2cache_link = sst.Link(prefix + ".link_l1dcache_l2cache_link")
        link_l1dcache_l2cache_link.connect((cpu_l1dcache, "lowlink", "1ns"),
                                           (cache_bus, "highlink0", "1ns"))
        link_l1dcache_l2cache_link.setNoCut()
        link_l1icache_l2cache_link = sst.Link(prefix + ".link_l1icache_l2cache_link")
        link_l1icache_l2cache_link.connect((cpu_l1icache, "lowlink", "1ns"),
                                           (cache_bus, "highlink1", "1ns"))
        link_l1icache_l2cache_link.setNoCut()
        link_bus_l2cache_link = sst.Link(prefix + ".link_bus_l2cache_link")
        link_bus_l2cache_link.connect((cache_bus, "lowlink0", "1ns"),
                                      (cpu_l2cache, "highlink", "1ns"))
        link_bus_l2cache_link.setNoCut()

        return ((cpu, "os_link", "5ns"),
                (l2cache_2_mem, "port", "1ns"),
                (dtlb, "mmu", "1ns"),
                (itlb, "mmu", "1ns"),
                hali)


node_os = sst.Component("os", lib + ".VanadisNodeOS")
node_os.addParams(osParams)
num = 0
for i, process in processList:
    for y in range(i):
        node_os.addParams(addParamsPrefix("process" + str(num), process))
        num += 1

node_os_mmu = node_os.setSubComponent("mmu", "mmu." + mmuType)
node_os_mmu.addParams(mmuParams)
node_os_mem_if = node_os.setSubComponent("mem_interface",
                                          "memHierarchy.standardInterface")
os_cache = sst.Component("node_os.cache", "memHierarchy.Cache")
os_cache.addParams(osl1cacheParams)
os_cache_2_mem = os_cache.setSubComponent("lowlink", "memHierarchy.MemNIC")
os_cache_2_mem.addParams(l2memLinkParams)

comp_chiprtr = sst.Component("chiprtr", "merlin.hr_router")
comp_chiprtr.addParams(memRtrParams)
comp_chiprtr.setSubComponent("topology", "merlin.singlerouter")
dirctrl = sst.Component("dirctrl", "memHierarchy.DirectoryController")
dirctrl.addParams(dirCtrlParams)
dirNIC = dirctrl.setSubComponent("highlink", "memHierarchy.MemNIC")
dirNIC.addParams(dirNicParams)
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams(memCtrlParams)
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams(memParams)

link_dir_2_rtr = sst.Link("link_dir_2_rtr")
link_dir_2_rtr.connect((comp_chiprtr, "port" + str(numCpus), "1ns"),
                       (dirNIC, "port", "1ns"))
link_dir_2_rtr.setNoCut()

ecc_guard = sst.Component("ecc_guard", "Carcosa.EccGuard")
ecc_guard.addParams({
    "verbose":                 "false",
    "state_key":               vla_state_key,
    "ecc_scheme":              ecc_scheme,
    "ber":                     ecc_ber,
    "correctable_latency_ps":  ecc_correctable_latency_ps,
    "due_latency_ps":          ecc_due_latency_ps,
    "escape_latency_ps":       ecc_escape_latency_ps,
    "kernel_policy":           ecc_kernel_policy,
    "apply_on_responses_only": ecc_apply_on_responses_only,
    "fault_model":             ecc_fault_model,
    "fault_mode_weights":      ecc_fault_mode_weights,
    "fault_event_rate":        ecc_fault_event_rate,
    "campaign_target_kernel":  ecc_campaign_target_kernel,
    "campaign_mode":           ecc_campaign_mode,
    "campaign_event_budget":   ecc_campaign_event_budget,
    "campaign_event_rate":     ecc_campaign_event_rate,
    "campaign_max_events_per_kernel_entry": ecc_campaign_max_per_entry,
    "campaign_errors_fixed":   ecc_campaign_errors_fixed,
    "campaign_force_multi_chip": ecc_campaign_force_multi_chip,
    "addr_filter_region":      ecc_addr_filter_region,
    "addr_filter_len":         ecc_addr_filter_len,
    "payload_dtype":           ecc_payload_dtype,
    "due_action":              ecc_due_action,
    "fit_per_mbit_per_hour":   ecc_fit_per_mbit_per_hour,
    "dram_capacity_mb":        ecc_dram_capacity_mb,
    "sim_time_per_event_ns":   ecc_sim_time_per_event_ns,
    "seed":                    ecc_seed,
})
ecc_guard.enableAllStatistics()

if action_scorer_enabled:
    scorer = sst.Component("action_scorer", "Carcosa.ActionScorer")
    scorer.addParams({
        "state_key":       vla_state_key,
        "golden_log":      action_scorer_golden,
        "emit_golden":     "true" if action_scorer_emit_golden else "false",
        "golden_required": "true" if action_scorer_golden_required else "false",
        "verbose":         "false",
    })
    scorer.enableAllStatistics()

if critical_watcher_enabled:
    crit_watcher = sst.Component("crit_watcher", "Carcosa.CriticalActionWatcher")
    crit_watcher.addParams({
        "state_key":               vla_state_key,
        "critical_region":         critical_watcher_region,
        "critical_len":            critical_watcher_len,
        "apply_on_responses_only": "true",
        "verbose":                 "false",
    })
    crit_watcher.enableAllStatistics()
    link_dir_2_watcher = sst.Link("link_dir_2_watcher")
    link_dir_2_watcher.connect((dirctrl, "lowlink", "1ns"), (crit_watcher, "highlink", "1ns"))
    link_dir_2_watcher.setNoCut()
    link_watcher_2_ecc = sst.Link("link_watcher_2_ecc")
    link_watcher_2_ecc.connect((crit_watcher, "lowlink", "1ns"), (ecc_guard, "highlink", "1ns"))
    link_watcher_2_ecc.setNoCut()
else:
    link_dir_2_ecc = sst.Link("link_dir_2_ecc")
    link_dir_2_ecc.connect((dirctrl, "lowlink", "1ns"), (ecc_guard, "highlink", "1ns"))
    link_dir_2_ecc.setNoCut()
link_ecc_2_mem = sst.Link("link_ecc_2_mem")
link_ecc_2_mem.connect((ecc_guard, "lowlink", "1ns"), (memctrl, "highlink", "1ns"))
link_ecc_2_mem.setNoCut()
link_os_cache_link = sst.Link("link_os_cache_link")
link_os_cache_link.connect((node_os_mem_if, "lowlink", "1ns"),
                           (os_cache, "highlink", "1ns"))
link_os_cache_link.setNoCut()
os_cache_2_rtr = sst.Link("os_cache_2_rtr")
os_cache_2_rtr.connect((os_cache_2_mem, "port", "1ns"),
                       (comp_chiprtr, "port" + str(numCpus + 1), "1ns"))
os_cache_2_rtr.setNoCut()

cpuDelayAgentParams = {
    "num_vit_layers":    vla_num_vit_layers,
    "num_llm_layers":    vla_num_llm_layers,
    "max_cycles":        vla_max_cycles,
    "initial_seq_len":   vla_initial_seq_len,
    "max_seq_len":       vla_max_seq_len,
    "num_action_tokens": vla_num_action_tokens,
    "decode_exit_prob":  vla_decode_exit_prob,
    "rng_seed":          vla_rng_seed,
    "baseline_ps":       vla_baseline_cpu_ps,
    "scale_factor":      vla_scale_factor,
    "state_key":         vla_state_key,
    "regions":           vla_regions,
    "verbose":           "true",
}
gpuDelayAgentParams = {
    "baseline_ps":     vla_baseline_gpu_ps,
    "scale_factor":    vla_scale_factor,
    "max_seq_len":     vla_max_seq_len,
    "verbose":         "true",
}

coreConfigs = [
    (cpuCoreParams, cpuLsqParams, cpu_l2cacheParams,
     "Carcosa.VLACpuDelayAgent", cpuDelayAgentParams),
    (gpuCoreParams, gpuLsqParams, gpu_l2cacheParams,
     "Carcosa.VLAGpuDelayAgent", gpuDelayAgentParams),
]

cpuBuilder = CPU_Builder()
nodeId = 0
halis = []
for cpu in range(numCpus):
    prefix = "node" + str(nodeId) + ".cpu" + str(cpu)
    cParams, lsqP, l2P, agentType, agentP = coreConfigs[cpu]
    os_hdlr, l2cache, dtlb, itlb, hali = cpuBuilder.build(
        prefix, cpu, cParams, lsqP, l2P, agentType, agentP)
    halis.append(hali)

    link_mmu_dtlb_link = sst.Link(prefix + ".link_mmu_dtlb_link")
    link_mmu_dtlb_link.connect(
        (node_os_mmu, "core" + str(cpu) + ".dtlb", "1ns"), dtlb)
    link_mmu_itlb_link = sst.Link(prefix + ".link_mmu_itlb_link")
    link_mmu_itlb_link.connect(
        (node_os_mmu, "core" + str(cpu) + ".itlb", "1ns"), itlb)
    link_core_os_link = sst.Link(prefix + ".link_core_os_link")
    link_core_os_link.connect(os_hdlr, (node_os, "core" + str(cpu), "5ns"))
    link_l2cache_2_rtr = sst.Link(prefix + ".link_l2cache_2_rtr")
    link_l2cache_2_rtr.connect(l2cache, (comp_chiprtr, "port" + str(cpu), "1ns"))

hali_ring_left = sst.Link("hali_ring_left")
hali_ring_left.connect((halis[0], "left", "5ns"), (halis[1], "right", "5ns"))
hali_ring_right = sst.Link("hali_ring_right")
hali_ring_right.connect((halis[0], "right", "5ns"), (halis[1], "left", "5ns"))
