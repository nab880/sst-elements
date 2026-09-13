"""A one-byte Chipkill tail occupies two x4 chips and cannot escape."""
from eccRuntimeCommon import build


# Even a draw containing many bit errors can only touch the two represented
# chips. Require correction and DUE outcomes so a disabled sampler cannot pass.
build({"ecc_scheme": "chipkill", "ber": 0.1,
       "test_total_min": 5000, "test_total_max": 5000,
       "test_correctable_min": 1, "test_due_min": 1,
       "test_escape_min": 0, "test_escape_max": 0},
      {"requests": 5000, "payload_size": 1, "expect_escapes": 0},
      name="partial_chipkill")
