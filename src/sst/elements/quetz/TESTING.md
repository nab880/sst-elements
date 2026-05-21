# Quetz — build and test guide

This document describes how to **build** the Quetz SST element and QEMU plugin and **run** the in-tree regression suite. The workflow below matches what was used to validate the compartmentalized layout on branch `quetz-refactor` (Docker image build + `sst-test-elements`).

For day-to-day development, use the **Docker** path in the [raptor](https://github.com/nab880/sst-elements) repo (`docker/` at the repository root that contains `sst-elements/`). A native host build is possible but not covered here.

---

## Prerequisites

| Requirement | Notes |
|-------------|--------|
| Docker | Desktop or Engine with enough disk (~8 GB for the image) and RAM |
| Git checkout | `sst-elements` tree with `src/sst/elements/quetz/` and `tests/` |
| Time (first build) | **~20–40 minutes** — QEMU 9.2, SST-Core, SST-Elements (`memHierarchy`, `mmu`, `quetz`) compile with `make -j2` inside the image |

The Docker image installs:

- **SST** at `/opt/sst` (`sst`, `sst-test-elements`, `libquetz.so`)
- **QEMU** at `/opt/qemu` (`qemu-riscv64`, `qemu-system-*`, plugin API headers)
- **`libqemu_sst_plugin.so`** at `/opt/sst/libexec/`

---

## Quick start (recommended)

From the **raptor repo root** (parent of `sst-elements/`):

```bash
./docker/build-and-test.sh
```

This script:

1. Builds the Docker image `raptor-quetz-test` from `docker/Dockerfile`
2. Runs the Quetz testsuite inside a container with `sst-elements/` mounted at `/src/sst-elements`

**Success** ends with:

```text
== TESTING PASSED ==
=== All Quetz tests passed ===
```

In a clean run you should see **11 tests passing** (usermode + sysmode + PR1 checks: multicore quorum, class balance, wide split, UART).

---

## Step-by-step (what the scripts do)

### 1. Build the test image (one-time or after Dockerfile changes)

```bash
cd /path/to/raptor
docker build -t raptor-quetz-test -f docker/Dockerfile .
```

Override the image name if needed:

```bash
export RAPTOR_QUETZ_IMAGE=my-quetz-test
docker build -t "${RAPTOR_QUETZ_IMAGE}" -f docker/Dockerfile .
```

The Dockerfile compiles QEMU with plugin support, then SST-Core and SST-Elements (including Quetz). Rebuild is required after changing **build system** files (`Makefile.am`, `configure.m4`) or when the image does not yet contain your sources.

### 2. Run the testsuite

**Option A — combined script (build + test):**

```bash
./docker/build-and-test.sh
```

**Option B — test only** (image already built; use live `sst-elements` sources on the host):

```bash
docker run --rm \
  -v "$(pwd)/sst-elements:/src/sst-elements" \
  raptor-quetz-test \
  /usr/local/bin/run-quetz-tests.sh
```

The mount means Python tests and guest binaries under `tests/` are read from your working tree. **C/C++ changes** still require the libraries inside the image to be rebuilt (see § Rebuild after code changes).

### 3. What runs inside the container

`docker/run-quetz-tests.sh` sets `SST_HOME=/opt/sst` and invokes:

```bash
sst-test-elements -p /src/sst-elements/src/sst/elements/quetz/tests/testsuite_default_quetz.py
```

It also prints QEMU versions and confirms `libquetz.so` / `libqemu_sst_plugin.so` are installed.

---

## Rebuild after code changes

| Change type | Action |
|-------------|--------|
| Python tests, SDL, gold files only | Re-run §2 Option B (no image rebuild) |
| Quetz C++ / plugin / `Makefile.am` | Rebuild image: `./docker/build-and-test.sh` or `docker build ...` |
| Faster iteration on C++ only | Rebuild SST-Elements layer inside a running container (advanced); simplest path is full `docker build` |

After a successful image build, a **test-only** run with the volume mount took on the order of **~10 seconds** in this session.

---

## Refresh gold files

The usermode tests compare SST stdout to `*.gold` files under `tests/usermode/small/<testname>/`. The harness uses `QuetzStatsFilter` in `tests/testsuite_default_quetz.py`, which diffs only selected ` cpu.*` event-count lines (latencies, cycle counts, `no_ops`, and `instruction_count` are ignored).

### When you need to update gold

Regenerate gold on the **same environment you use for CI** (the Docker image below) whenever:

- You change Quetz IPC, the QEMU plugin, or the testsuite (e.g. `QUETZ_DETAILED`, SDL, or filter rules) and usermode tests fail on **stat mismatches** while sysmode / multicore / class-balance tests still pass.
- You first validate Quetz on a new platform (e.g. aarch64 Docker vs the machine that produced the checked-in gold).
- QEMU or statistic output changes **intentionally** and the new numbers are correct.

A failing usermode test with a fast exit (~0.2 s), guest `Hello World` in the log, and `Plugin attached!` is usually a **gold drift** problem, not a broken simulation. Do **not** refresh gold to mask a hang or plugin load error.

### How to update gold (next time)

1. **Build the image** so `/opt/sst` contains your C++ / plugin changes (gold refresh alone is not enough after `Makefile.am` or tunnel changes):

   ```bash
   ./docker/build-and-test.sh
   ```

   Expect some failures on the first run if gold is stale; that is normal.

2. **Regenerate gold** from the same Docker image and mounted `sst-elements` tree:

   ```bash
   UPDATE_GOLD=1 ./docker/build-and-test.sh
   ```

   Or, if the image is already built:

   ```bash
   UPDATE_GOLD=1 docker run --rm \
     -v "$(pwd)/sst-elements:/src/sst-elements" \
     raptor-quetz-test \
     /usr/local/bin/run-quetz-tests.sh
   ```

   This temporarily sets `updateFiles = True` in `tests/testsuite_default_quetz.py`, runs the suite, copies each test’s stdout into the reference `sst.stdout.gold`, then restores `updateFiles = False`.

3. **Review and commit** the updated files under `tests/usermode/small/` (and `tests/sysmode/` if any sysmode gold was refreshed). Re-run without `UPDATE_GOLD` and confirm `== TESTING PASSED ==` (11 tests).

Gold updates are **not** required for every refactor; only when the filtered stat lines above legitimately change. Skip this step during exploratory work until you are ready to lock in new reference output.

---

## Testsuite layout

| Path | Role |
|------|------|
| `tests/testsuite_default_quetz.py` | Main harness; matrices for usermode/sysmode |
| `tests/usermode/` | User-mode QEMU (`qemu-riscv64`, `qemu-aarch64`, `qemu-x86_64`, …) |
| `tests/sysmode/` | System-mode (`qemu-system-*`), firmware, memmap |
| `tests/binaries/` | Guest ELFs used by SDL scripts |
| `tests/sst_test_outputs/` | Generated output (gitignored in normal use) |

Individual tests are Python files that build an SST graph and compare output to `*.gold` files (event-count stats are stable; some timing fields are filtered).

---

## Unit tests (C++, no QEMU/SST simulation)

Fast checks for decoders, IPC layout, region lookup, cache-line splitting, and platform presets.

```bash
# Inside Docker or any host with SST headers at $SST_HOME/include
export SST_HOME=/opt/sst
make -C src/sst/elements/quetz check-quetz-unit
# or directly:
src/sst/elements/quetz/tests/unit/run_unit_tests.sh
```

Add a case in `tests/unit/test_decoder_riscv.cc` (or a new `test_*.cc`), list it in `run_unit_tests.sh`, rebuild.

---

## Verify install manually (optional)

Inside the container:

```bash
docker run --rm -it raptor-quetz-test bash
export PATH=/opt/sst/bin:$PATH
export LD_LIBRARY_PATH=/opt/sst/lib
ls -la /opt/sst/lib/sst-elements-library/libquetz.so
ls -la /opt/sst/libexec/libqemu_sst_plugin.so
qemu-riscv64 --version
sst-test-elements -p /src/sst-elements/src/sst/elements/quetz/tests/testsuite_default_quetz.py
```

---

## Troubleshooting

| Symptom | Likely cause |
|---------|----------------|
| `qemu_plugin path not specified` / plugin not found | `libqemu_sst_plugin.so` not installed; rebuild image |
| QEMU exits immediately on start | Wrong `qemu` binary (static build without `-plugin`); image uses dynamic QEMU 9.2 |
| Compile error in `quetz_stats` / `registerStatistic` | Stats must register from `QuetzCore` (friend) or its methods — not from a free function |
| Tests pass in image but fail on host | Host SST/QEMU versions differ; use Docker for canonical results |
| Usermode FAIL in ~0.2 s, sysmode PASS | Stat lines differ from gold; rebuild image, then `UPDATE_GOLD=1` (see § Refresh gold files) |
| Build OOM | Dockerfile uses `make -j2`; increase Docker memory or reduce parallelism further |
| First build very slow | Expected: full QEMU + SST compile |

---

## Native build (outline only)

If you already have SST-Core, SST-Elements dependencies, and QEMU **with** `qemu-plugin.h` on the host:

```bash
cd sst-elements
./autogen.sh
mkdir -p build && cd build
../configure --prefix=$SST_HOME --with-sst-core=$SST_HOME --with-qemu-prefix=/path/to/qemu
make -j$(nproc) install
export PATH=$SST_HOME/bin:$PATH
sst-test-elements -p src/sst/elements/quetz/tests/testsuite_default_quetz.py
```

Quetz `configure` must find `qemu-plugin.h` and a plugin-capable QEMU. The Docker path above is the supported way to get a matching toolchain without manual alignment.

---

## Related docs

- [README.md](README.md) — component usage, parameters, QEMU version table
- [QUETZ_OUTLINE.md](QUETZ_OUTLINE.md) — architecture and source map
- [`docker/README.md`](../../../../../docker/README.md) — Docker image details (raptor repo root)
