"""Poisson data correction, escaping corruption, and scheduled ECC latency."""
from eccRuntimeCommon import build

build({"ecc_scheme": "secded", "ber": 0,
       "correctable_latency_ps": 1000, "due_latency_ps": 1000,
       "escape_latency_ps": 1000,
       "test_total_min": 10, "test_total_max": 10,
       "test_clean_min": 10, "test_clean_max": 10},
      {"requests": 10, "expect_mutated": 0, "expect_escapes": 0,
       "test_elapsed_ps_min": 40000, "test_elapsed_ps_max": 40000},
      name="clean")

# 1000 independent 64-bit words at lambda=.64 yield about 337 corrected reads.
# Other outcomes add no delay, so the RTT bound checks correction latency itself.
build({"ecc_scheme": "secded", "ber": .01,
       "correctable_latency_ps": 1000,
       "test_total_min": 1000, "test_total_max": 1000,
       "test_correctable_min": 275, "test_correctable_max": 400},
      {"requests": 1000, "test_min_changed_bits": 2,
       "test_elapsed_ps_min": 4275000, "test_elapsed_ps_max": 4400000},
      name="correctable")

# Unprotected words at lambda=8 almost always escape. Only escapes add latency.
build({"ecc_scheme": "none", "ber": .125, "escape_latency_ps": 1000,
       "test_total_min": 100, "test_total_max": 100,
       "test_escape_min": 95, "test_escape_max": 100},
      {"requests": 100, "test_mutated_min": 95, "test_mutated_max": 100,
       "test_elapsed_ps_min": 495000,
       "test_elapsed_ps_max": 500000},
      name="escape")
