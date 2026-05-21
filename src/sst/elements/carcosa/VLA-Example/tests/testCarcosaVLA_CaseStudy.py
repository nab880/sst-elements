"""Section 4 case-study SST config.

Identical to testCarcosaVLA_GPUCPU_Synth.py; injection knobs come from the
environment (see scripts/run_case_studies.sh and case_studies/manifest.yaml).

  CASE_ID, ECC_SCHEME, ECC_CAMPAIGN_*, CRITICAL_ACTION_WATCHER, etc.
"""
import os
import runpy

_here = os.path.dirname(os.path.abspath(__file__))
runpy.run_path(os.path.join(_here, "testCarcosaVLA_GPUCPU_Synth.py"), run_name="__main__")
