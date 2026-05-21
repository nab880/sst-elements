#!/usr/bin/env bash
# Phase-1 verification: run the headline 15-case matrix (seed=1, 3 schemes,
# default latency profile) and apply provisional acceptance checks.
#
# Thresholds are placeholders pending the first full calibration run on
# real binaries; the Phase-2 (synth) thresholds in the manifest were
# tuned against synthetic delay-agent traffic and do not translate
# directly to vla_cpu/vla_gpu. Update manifest.yaml::acceptance after the
# calibrate-acceptance step in the plan.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)/case_studies}"

export PREFLIGHT=1
export OUT_DIR
export SEEDS=1
export SCHEMES="none secded chipkill"

# Clear prior run logs but keep manifest.yaml and goldens.
mkdir -p "$OUT_DIR/goldens"
find "$OUT_DIR" -maxdepth 1 -name 'case_*.log' -delete 2>/dev/null || true
[ -f "$OUT_DIR/index.csv" ] && mv "$OUT_DIR/index.csv" "$OUT_DIR/index.csv.bak" 2>/dev/null || true

"$SCRIPT_DIR/run_case_studies.sh" || exit 1

# Both analyzers run inside run_case_studies.sh on completion; rerun here
# explicitly so a stale analysis/ from a prior PREFLIGHT can't mask a real
# regression.
python3 "$SCRIPT_DIR/analyze_ecc_results.py" "$OUT_DIR" --case-studies || exit 1
python3 "$SCRIPT_DIR/propagation_analyzer.py" "$OUT_DIR" || exit 1

TABLE="$OUT_DIR/analysis/case_study_table.csv"
PROP="$OUT_DIR/analysis/propagation_summary.csv"
if [ ! -s "$TABLE" ]; then
    echo "ERROR: missing $TABLE" >&2
    exit 1
fi
if [ ! -s "$PROP" ]; then
    echo "ERROR: missing $PROP" >&2
    exit 1
fi

python3 - "$TABLE" "$PROP" <<'PY'
import csv, sys
table_rows = list(csv.DictReader(open(sys.argv[1])))
prop_rows  = list(csv.DictReader(open(sys.argv[2])))

def cell(rows, **kw):
    """Return first row matching all kw filters or None."""
    for r in rows:
        if all(str(r.get(k, '')) == str(v) for k, v in kw.items()):
            return r
    return None

fail   = []
warn   = []

# === Hard checks that must hold on any reasonable Phase-1 calibration ===

# 1. C0 (no injection) should be silent on every scheme: 0 events, 0 escapes,
#    8 frames all classified O1. If any C0 cell shows escapes the fault
#    plumbing is misconfigured, not just mis-calibrated.
for r in [x for x in table_rows if x['case_id'] == 'C0']:
    if int(r.get('events_total', 0)) != 0:
        fail.append(f"C0 {r['scheme']} events_total={r['events_total']} (want 0)")
    if float(r.get('unsafe_action_rate', 0)) > 0.0:
        fail.append(f"C0 {r['scheme']} unsafe={r['unsafe_action_rate']} (want 0)")
    if float(r.get('fraction_O1', 0)) < 1.0:
        warn.append(f"C0 {r['scheme']} O1 fraction={r['fraction_O1']} (want 1.0)")

# 2. Injected cases (C1..C4) on `none` should record at least one ECC event;
#    if events_total is still 0 the campaign filter never matched the
#    binary-registered action_queue region.
for cid in ('C1', 'C2', 'C3', 'C4'):
    r = cell(table_rows, case_id=cid, scheme='none')
    if r is None:
        warn.append(f"{cid} none: missing row")
        continue
    if int(r.get('events_total', 0)) == 0:
        fail.append(f"{cid} none events_total=0; campaign never fired "
                    "(check addr_filter_region + binary region registration)")

# 3. Propagation summary: C0 must have zero O2/O3/O4 frames; injected cases
#    should propagate to SOME channel.
for r in prop_rows:
    cid, sch = r['case_id'], r['scheme']
    o234 = (int(r['frames_outcome_O2'])
            + int(r['frames_outcome_O3'])
            + int(r['frames_outcome_O4']))
    if cid == 'C0' and o234 != 0:
        fail.append(f"C0 {sch} propagation O2+O3+O4={o234} (want 0)")
    if cid in ('C1', 'C2', 'C3', 'C4') and sch == 'none' and o234 == 0:
        warn.append(f"{cid} {sch} propagation O2+O3+O4=0; no fault reached a "
                    "scored channel")

# === Soft (calibration) checks: warnings only until thresholds are tuned ===

def rate(case, scheme, lat='default'):
    r = cell(table_rows, case_id=case, scheme=scheme, latency_profile=lat)
    if r is None:
        return None
    return float(r.get('unsafe_action_rate', 0) or 0)

# Pareto / latency profile checks intentionally dropped: this branch only
# runs the manifest default latency profile.

# Provisional: chipkill should at least not be worse than secded at the same
# case, since chipkill strictly subsumes secded coverage.
for cid in ('C1', 'C2'):
    ck = rate(cid, 'chipkill')
    sd = rate(cid, 'secded')
    if ck is not None and sd is not None and ck > sd + 1e-9:
        warn.append(f"{cid} chipkill unsafe ({ck:.3f}) > secded unsafe ({sd:.3f}); "
                    "either expected with current calibration or a real regression")

# Provisional: C4 (multi-chip chipkill defeat) should propagate more than C1
# (single-bit). If it doesn't, the manifest's force_multi_chip ceiling needs
# tuning.
for sch in ('none', 'secded', 'chipkill'):
    u1 = rate('C1', sch)
    u4 = rate('C4', sch)
    if u1 is not None and u4 is not None and u4 < u1:
        warn.append(f"C4 {sch} unsafe ({u4:.3f}) < C1 {sch} unsafe ({u1:.3f}); "
                    "C4 should be the multi-chip ceiling")

if warn:
    print("PREFLIGHT WARNINGS (provisional, recalibrate manifest):")
    for w in warn:
        print(" ", w)

if fail:
    print("PREFLIGHT FAILED:")
    for f in fail:
        print(" ", f)
    sys.exit(1)
print("PREFLIGHT OK: hard checks passed; review warnings above before "
      "promoting thresholds.")
PY
