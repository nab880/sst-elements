"""
Same Vanadis + memHierarchy + Hali + FourStateAgent stack as
testFourStateRegistry.py, but with a PortModuleStateGate installed on each
Hali's lowlink port. The gate reads the FourStateAgent-published snapshot
in PipelineStateRegistry<PipelineStateBase> for the matching state_key and,
while the agent's currentKernel is in the configured set, probabilistically
flips a single byte in MemEvent payloads that transit the lowlink.

INSTRUCTIONS ARE NEVER CORRUPTED. The wiring is:
    CPU.icache <--> iTLB <--> L1I             (iCache path, no Hali)
    CPU.dcache <--> Hali(highlink <-> lowlink) <--> dTLB <--> L1D

The gate sits on Hali's lowlink in Receive direction; the L1I path does not
pass through Hali at all, so instruction fetches physically cannot be
flipped by this gate. Any instruction-decode fatal the CPU might hit is
purely an indirect effect of a corrupted DATA byte later being consumed as
a branch target / return address / function pointer.

Scoping notes:
  - FourStateAgent intercepts MMIO reads/writes at Hali's HIGHLINK (where
    CPU dcache traffic enters Hali) and responds locally via Hali's
    `interceptionAgent` subcomponent. MMIO command traffic therefore NEVER
    leaves Hali toward the L1D, so the gate on LOWLINK only ever sees
    non-MMIO data-cache traffic (heap, stack, page tables, checksum
    storage). This is a feature: it lets us perturb ordinary memory
    without touching the MMIO command protocol itself.
  - `region_names="mmio_control"` would therefore *reject every event* if
    its per-event address filter were enabled on lowlink. We intentionally
    do NOT use it here; the `kernels="2"` state predicate is what scopes
    the fault window.

Gate config:
  - fault_mode=flip            : corrupt one payload byte instead of drop.
  - flip_probability=0.001     : rare enough that the ~hundreds of dcache
                                 events per kernel window see only a
                                 handful of flips, so the CPU protocol
                                 still makes forward progress. Combined
                                 with the fixed seed this gives a
                                 deterministic run.
  - kernels="2"                : engage only while currentKernel == 2
                                 (the read-modify-write kernel in
                                 fourstate.c). K0/K1/K3 dcache traffic is
                                 untouched.
  - install_direction=Receive  : intercept responses coming back from the
                                 cache toward the CPU.
  - log_injections=1           : emit a line to stdout on every flip
                                 showing sim-time address, payload size,
                                 and running counts (matched/flipped/
                                 dropped/filtered). Grep for
                                 "carcosa.PortModuleStateGate" to see
                                 every injected memory fault.
  - seed=42/43                 : fixed per-core. FaultInjectorBase
                                 defaults to seed=0 which maps to a
                                 wall-clock-seeded MersenneRNG -- without
                                 a fixed seed runs are non-reproducible
                                 and occasionally (~10-20%) livelock or
                                 trigger a Vanadis fatal.

Expected output (the point of the test):
    carcosa.PortModuleStateGate[core0] FLIP addr=0x... payload=N \
        matched=1 flipped=1 dropped=0 filtered=0
    carcosa.PortModuleStateGate[core0] FLIP addr=0x... payload=N \
        matched=2 flipped=2 dropped=0 filtered=0
    ...
The number of FLIP lines is the number of memory faults injected into
dcache responses during K2 windows. Zero FATAL lines means the Vanadis
core retired those faulted loads without a control-flow fatal.

Build fourstate.c (same prereq as the ungated test):
  riscv64-unknown-linux-gnu-gcc -static -I.. -o fourstate fourstate.c

Run:
  sst testFourStateRegistryGated.py 2>&1 | grep PortModuleStateGate
"""

import importlib
_base = importlib.import_module("testFourStateRegistry")

_COMMON_GATE = {
    "fault_mode":          "flip",
    "flip_probability":    "0.001",
    "kernels":             "2",
    "install_direction":   "Receive",
    "log_injections":      "1",
    "verbose":             "1",
}

_base.GATE_PARAMS_PER_CORE = {
    "core0": {"state_key": "core0", "seed": "42", **_COMMON_GATE},
    "core1": {"state_key": "core1", "seed": "43", **_COMMON_GATE},
}

_base.build_topology()
