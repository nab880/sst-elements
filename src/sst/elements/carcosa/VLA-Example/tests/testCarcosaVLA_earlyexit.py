# Regression coverage for VlaFsm::decodeEarlyExitProb.
#
# Runs the same single-CPU VLA workload as testCarcosaVLA.py but configures
# the VLAAgent with:
#   num_action_tokens=8     (hard cap on decode loop)
#   decode_exit_prob=0.25   (Bernoulli early-exit per LM_HEAD step)
#   rng_seed=42             (fixed for reproducibility across runs)
#
# Expected in the agent's verbose trace (stdout):
#   * Multiple "LM_HEAD -> DETOK_DEQUANT" transitions where the token
#     count is < 8 (proves the coin flip terminated decoding early).
#   * Simulation completes cleanly; no FSM fatal.
#
# Drives the main config by setting env vars and exec'ing it; this avoids
# duplicating the ~270-line SST topology setup in testCarcosaVLA.py.

import os

os.environ.setdefault("VLA_NUM_ACTION_TOKENS", "8")
os.environ.setdefault("VLA_DECODE_EXIT_PROB", "0.25")
os.environ.setdefault("VLA_RNG_SEED", "42")

_here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(_here, "testCarcosaVLA.py")) as _f:
    exec(compile(_f.read(), "testCarcosaVLA.py", "exec"))
