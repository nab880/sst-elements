# Regression coverage for VlaFsm::decodeEarlyExitProb.

import os

os.environ.setdefault("VLA_NUM_ACTION_TOKENS", "8")
os.environ.setdefault("VLA_DECODE_EXIT_PROB", "0.25")
os.environ.setdefault("VLA_RNG_SEED", "42")

_here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(_here, "testCarcosaVLA.py")) as _f:
    exec(compile(_f.read(), "testCarcosaVLA.py", "exec"))
