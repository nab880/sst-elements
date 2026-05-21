#!/usr/bin/env bash
# Goldilocks pilot: C1 x scheme=none x budgets/rates (see manifest acceptance band).
exec env PILOT=1 SEEDS=1 SCHEMES=none "$(dirname "$0")/run_case_studies.sh"
