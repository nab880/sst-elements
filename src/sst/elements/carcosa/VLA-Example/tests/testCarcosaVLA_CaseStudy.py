"""Section 4 case-study SST config (Phase 1 / real binaries).

Thin wrapper around testCarcosaVLA_GPUCPU.py. All injection knobs come from
the environment, set by scripts/run_case_studies.sh based on
case_studies/manifest.yaml:

  CASE_ID, ECC_SCHEME, ECC_FAULT_MODEL=campaign, ECC_CAMPAIGN_*,
  ECC_ADDR_FILTER_REGION, CRITICAL_ACTION_WATCHER,
  ACTION_SCORER_GOLDEN, VLA_REGIONS, ...

Before the ecc_all_p1 branch this wrapper sourced testCarcosaVLA_GPUCPU_Synth
(Phase 2, synthetic delay agents); see git history if you need the synthetic
sweeps.
"""
import os
import runpy

_here = os.path.dirname(os.path.abspath(__file__))
runpy.run_path(os.path.join(_here, "testCarcosaVLA_GPUCPU.py"), run_name="__main__")
