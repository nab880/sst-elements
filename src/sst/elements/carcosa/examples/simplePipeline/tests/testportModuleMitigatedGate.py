"""
Exercise PortModuleMitigatedGate against SimplePipelineProducer.

Mirrors testportModuleStateGate.py but layers the mitigation model on top
of the same gate decision. Three scenarios of the same topology verify
the three observable behaviors that the paper's Claim 4 sweep relies on:

  1. mitigation_scheme="off"
     -> Identical to the base PortModuleStateGate: every event destined for
        stages in `kernels` is dropped. cycles_overhead=0.

  2. mitigation_scheme="secded", mask_probability=1.0
     -> Gate matches exactly as before, but every matched event is masked
        (fault suppressed). Sink sees *all* events; the final log line
        shows events_matched == total drop-eligible events and
        events_masked == events_matched. cycles_overhead == 2 * matched.

  3. mitigation_scheme="selective_secded"
     -> Parent gate predicates (`kernels="1,3"`) select which events are
        protected. Unmatched events bypass the scheme entirely -- no
        masking AND no overhead. This is the Claim 4 architecture lever:
        protection and its cost scale with the predicate set.

Run:
  sst testportModuleMitigatedGate.py
  sst testportModuleMitigatedGate.py -- secded
  sst testportModuleMitigatedGate.py -- selective_secded

The scheme defaults to "off" when no argument is given so the test is
drop-equivalent and yields the same histogram as the base-gate test.
"""

import sys
import sst

# See testportModuleStateGate.py for the memHierarchy side-effect import rationale.
try:
    import sst.memHierarchy  # noqa: F401
except ModuleNotFoundError:
    pass

sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "0 ns")

STATE_KEY    = "mitigated_gate_test"
TOTAL_CYCLES = 8

scheme = "off"
for arg in sys.argv[1:]:
    if arg in ("off", "secded", "dmr", "checkpoint", "selective_secded"):
        scheme = arg
        break

producer = sst.Component("producer", "Carcosa.SimplePipelineProducer")
producer.addParams({
    "state_key":    STATE_KEY,
    "total_cycles": TOTAL_CYCLES,
    "clock":        "1MHz",
    "verbose":      "true",
})

sink = sst.Component("sink", "Carcosa.SimplePipelineSink")
sink.addParams({"verbose": "true"})

sink.addPortModule("in", "carcosa.PortModuleMitigatedGate", {
    "state_key":           STATE_KEY,
    "fault_mode":          "drop",
    "drop_probability":    "1.0",
    "kernels":             "1,3",           # DECODE + COMMIT (post-link shift; see base test)
    "mitigation_scheme":   scheme,
    "ecc_check_cycles":    "2",
    "dmr_compute_cycles":  "100",
    "checkpoint_rollback_cycles": "10000",
    "verbose":             "1",
    "debug":               "1",
    "debug_level":         "2",
})

link = sst.Link("producer_to_sink")
link.connect((producer, "out", "1us"),
             (sink,     "in",  "1us"))
