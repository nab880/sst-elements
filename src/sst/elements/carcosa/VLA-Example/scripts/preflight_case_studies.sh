#!/usr/bin/env bash
# Verification-only case-study run (seed=1) + acceptance checks.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)/case_studies}"

export PREFLIGHT=1
export OUT_DIR
export SEEDS=1
export SCHEMES="none secded chipkill"

# Clear prior run logs but keep manifest.yaml
mkdir -p "$OUT_DIR/goldens"
find "$OUT_DIR" -maxdepth 1 -name 'case_*.log' -delete 2>/dev/null || true
[ -f "$OUT_DIR/index.csv" ] && mv "$OUT_DIR/index.csv" "$OUT_DIR/index.csv.bak" 2>/dev/null || true

"$SCRIPT_DIR/run_case_studies.sh" || exit 1

python3 "$SCRIPT_DIR/analyze_ecc_results.py" "$OUT_DIR" --case-studies || exit 1

TABLE="$OUT_DIR/analysis/case_study_table.csv"
if [ ! -s "$TABLE" ]; then
    echo "ERROR: missing $TABLE" >&2
    exit 1
fi

python3 - "$TABLE" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
def rate(case, scheme, lat="default"):
    for r in rows:
        if r["case_id"]==case and r["scheme"]==scheme and r.get("latency_profile","default")==lat:
            return float(r["unsafe_action_rate"])
    return None

fail = []
c0 = [r for r in rows if r["case_id"]=="C0"]
for r in c0:
    if float(r["unsafe_action_rate"]) > 0.01:
        fail.append(f"C0 {r['scheme']} unsafe={r['unsafe_action_rate']}")

u1 = rate("C1","none")
if u1 is None or u1 < 0.30:
    fail.append(f"C1 none unsafe={u1} (want >=0.30)")
u1s = rate("C1","secded")
if u1s is not None and u1s > 0.10:
    fail.append(f"C1 secded unsafe={u1s} (want <=0.10)")

u2s = rate("C2","secded")
if u2s is not None and u2s > 0.10:
    fail.append(f"C2 secded unsafe={u2s}")
o2 = next((float(r["fraction_O2"]) for r in rows if r["case_id"]=="C2" and r["scheme"]=="secded" and r.get("latency_profile")=="default"), 0)
if o2 < 0.25:
    fail.append(f"C2 secded O2 fraction={o2} (want >=0.25)")
u2c = rate("C2","chipkill")
if u2c is not None and u2c > 0.10:
    fail.append(f"C2 chipkill unsafe={u2c}")

for case in ("C4",):
    for sch in ("none","secded","chipkill"):
        u = rate(case, sch)
        if u is not None and u < 0.30:
            fail.append(f"{case} {sch} unsafe={u} (want >=0.30 ceiling)")

# Pareto: secded@strict vs chipkill@default on C2
def lat_ps(case, scheme, lat):
    for r in rows:
        if r["case_id"]==case and r["scheme"]==scheme and r.get("latency_profile")==lat:
            return float(r["ecc_latency_ps"]), float(r["unsafe_action_rate"])
    return None, None
lat_ck, u_ck = lat_ps("C2","chipkill","default")
lat_ss, u_ss = lat_ps("C2","secded","strict")
lat_n, u_n = lat_ps("C2","none","default")
if lat_ck and lat_ss and lat_ss < lat_ck and u_ss is not None and u_n is not None and u_ss < u_n:
    pass
elif lat_ck and lat_ss:
    fail.append(f"C2 Pareto check: secded@strict lat={lat_ss} unsafe={u_ss} vs chipkill lat={lat_ck}")

if fail:
    print("PREFLIGHT FAILED:")
    for f in fail:
        print(" ", f)
    sys.exit(1)
print("PREFLIGHT OK: all acceptance checks passed.")
PY
