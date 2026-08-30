# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

import sst


sst.setProgramOption("timebase", "1ps")
sst.Component("test", "firefly.allreduce_error_regression_test")
