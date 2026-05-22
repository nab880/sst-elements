# Quetzalcoatl (Quetz) — QEMU-backed CPU component for SST

Quetzalcoatl (Quetz)  is an SST processor component that uses a
QEMU TCG plugin to trace memory-access events from a guest binary and replay
them through SST's `memHierarchy` stack.  This lets you measure cache and
memory behaviour of real binaries — including those using extended ISAs like
RISC-V Vector (RVV) — without a full cycle-accurate pipeline model.

The design mirrors `ariel` (the Pin-based component) but uses QEMU instead
of Pin, making it portable to any architecture that QEMU supports in
user-mode and allowing custom ISA extensions via QEMU's TCG plugin API.

---

## How it works

```
┌─────────────────────────────────────────────────────┐
│  SST simulation process                             │
│                                                     │
│   QuetzCPU  ←──────────────── shared memory ──────────┐
│     └── QuetzCore[0]  →  memHierarchy.Cache  →  Mem │ │
│     └── QuetzCore[1]  →  memHierarchy.Cache  →  Mem │ │
│                                                     │ │
└─────────────────────────────────────────────────────┘ │
                                                        │
┌───────────────────────────────────────────────────────┘
│  QEMU child process (forked by QuetzCPU)
│
│   qemu-riscv64  -plugin libqemu_sst_plugin.so  <binary>
│
│   TCG plugin intercepts every load/store and instruction,
│   writes events into the shared-memory ring buffer.
└───────────────────────────────────────────────────────
```

On each SST clock tick every `QuetzCore` drains events from its ring-buffer
slot, applies optional per-class execution stalls, and issues
`StandardMem::Read` / `StandardMem::Write` requests to the attached memory
interface.  QEMU back-pressure is natural: the ring buffer is bounded, so
QEMU blocks (spins) whenever SST falls behind.

---

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| SST-core | 13+ | `sst` and `sst-config` on `$PATH` |
| sst-elements | this branch | `memHierarchy` element required |
| QEMU | 8+ | `qemu-<target>` or `qemu-system-<target>`; 9.0+ recommended (see below) |

The QEMU user-mode binary must support the `-plugin` flag.  Static builds
(`qemu-riscv64-static`) do **not** support plugins — use a
dynamically-linked build.

**QEMU 9.0+ is recommended.**  On older QEMU, store-data capture in
**system mode** is disabled because the v1-v3 fallback (dereferencing the
guest virtual address as a host pointer) only works in user mode.  QEMU 9.0
introduces `qemu_plugin_mem_get_value`, which reads the actual stored
value from the CPU's register file and works in both user and system mode.
On QEMU 8.x running user mode everything still works; only sysmode store
payloads (e.g. UART TX capture) require 9.0+.

The plugin source uses `QEMU_PLUGIN_VERSION` guards to handle API changes
across QEMU releases:

| QEMU version | Plugin API version | Notes |
|---|---|---|
| 8.x | v1 | `qemu_plugin_n_vcpus()`, `qemu_plugin_insn_data()` returns `const void*`; sysmode store-data disabled |
| 9.0 | v2 | `qemu_plugin_num_vcpus()` replaces `n_vcpus` |
| 9.1+ | v3 | `qemu_plugin_insn_data(insn, buf, len)` write-into-buffer API |
| 9.0+ | v4 | `qemu_plugin_mem_get_value()` — Quetz uses this for store-data capture (sysmode supported) |
| 10+ | v5 | Added memory read/write helpers (no breaking change for Quetz) |
| 11+ | v6 | Added discontinuity/syscall filter callbacks (no breaking change for Quetz) |

---

## Build

Quetz is built as part of `sst-elements`.  The configure step locates
`qemu-plugin.h` from the QEMU installation; pass `--with-qemu-prefix` if
QEMU is not installed in a standard system path.

```bash
# Configure sst-elements (from the sst-elements source directory)
./configure --prefix=$SST_HOME \
            SST_CORE_HOME=$SST_HOME \
            --with-qemu-prefix=/path/to/qemu/install

# Build and install
make -j$(nproc) install
```

This produces two install targets:

| File | Location | Purpose |
|---|---|---|
| `libquetz.so` | `$SST_HOME/lib/sst-elements-library/` | SST element library |
| `libqemu_sst_plugin.so` | `$SST_HOME/libexec/` | QEMU TCG plugin |

---

## Quick start

```python
import sst, os

sst_home = os.environ.get("SST_HOME", "/usr/local")

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams({
    "clock"       : "1GHz",
    "vcpu_count"  : 1,
    "qemu"        : os.path.join(sst_home, "bin", "qemu-riscv64"),
    "qemu_plugin" : os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"),
    "executable"  : "/path/to/your/riscv64/binary",
})

memctrl = sst.Component("mem", "memHierarchy.MemController")
memctrl.addParams({
    "clock"            : "1GHz",
    "addr_range_start" : 0,
    "addr_range_end"   : (1 << 48) - 1,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns", "mem_size": "256TiB"})

sst.Link("cpu_mem").connect(
    (cpu,     "cache_link_0", "1ns"),
    (memctrl, "highlink",     "1ns"))
```

---

## Component parameters

### Core parameters

| Parameter | Default | Description |
|---|---|---|
| `verbose` | `0` | Verbosity (0 = quiet) |
| `clock` | `1GHz` | CPU clock rate |
| `vcpu_count` | `1` | Guest vCPUs / hardware threads |
| `maxcorequeue` | `64` | Depth of the per-vCPU staging queue |
| `maxtranscore` | `16` | Max in-flight transactions per vCPU |
| `maxissuepercycle` | `1` | Max cache requests per cycle per vCPU |
| `cachelinesize` | `64` | Cache line size in bytes |

### QEMU / guest binary

| Parameter | Default | Description |
|---|---|---|
| `executable` | *(required)* | Path to the RISC-V ELF binary |
| `qemu` | `qemu-riscv64` | Path to the QEMU user-mode binary |
| `qemu_plugin` | *(auto)* | Path to `libqemu_sst_plugin.so`; auto-resolved from `$SST_HOME/libexec/` if empty |
| `qemu_args` | `""` | Extra QEMU flags before `-plugin` (e.g. `-L /opt/sysroot`) |
| `appargcount` | `0` | Number of arguments to pass to the binary |
| `apparg0` … `appargN` | `""` | Individual binary arguments |
| `appstdin` | *(inherit)* | Redirect guest stdin from file |
| `appstdout` | *(inherit)* | Redirect guest stdout to file |
| `appstderr` | *(inherit)* | Redirect guest stderr to file |
| `max_insts` | `0` | Halt after this many guest instructions per vCPU (0 = run to completion) |
| `checkaddresses` | `0` | If 1, warn when a single access spans more than one cache line |
| `envparamcount` | `-1` | Extra env vars to set (`-1` = inherit all) |
| `envparamname0`, `envparamval0`, … | `""` | Extra env var name/value pairs |

### Architecture properties

These are informational in user-mode (QEMU detects extensions from the
binary automatically) but are logged at startup and used by some external
tools.

| Parameter | Default | Description |
|---|---|---|
| `isa` | `""` | ISA string, e.g. `rv64gcv` |
| `has_fpu` | `0` | 1 if the modeled arch has an FPU |
| `has_vector` | `0` | 1 if the modeled arch has a vector unit |
| `vector_vlen` | `128` | Vector register length in bits (RISC-V VLEN) |
| `vector_elen` | `64` | Maximum vector element width in bits (RISC-V ELEN) |

### Execution latency model

Each instruction class can add extra stall cycles before its load/store
reaches the cache — useful for modelling functional-unit pipeline depth.

| Parameter | Default | Description |
|---|---|---|
| `exec_latency_int` | `0` | Extra cycles for integer load/stores |
| `exec_latency_fp` | `0` | Extra cycles for scalar FP load/stores |
| `exec_latency_vec` | `0` | Extra cycles for vector load/stores |
| `detailed_instruction_tracking` | `0` | If 1, populate per-class non-memory instruction statistics (`int_compute`, `fp_compute`, `vec_compute`, `branch`). Requires a RISC-V or AArch64 guest; other ISAs emit a warning and report all non-memory instructions as OTHER. |
| `compute_latency_int` | `0` | Extra cycles an integer compute NOP occupies the issue queue (requires `detailed_instruction_tracking=1`) |
| `compute_latency_fp` | `0` | Extra cycles a scalar FP compute NOP occupies the issue queue (requires `detailed_instruction_tracking=1`) |
| `compute_latency_vec` | `0` | Extra cycles a vector compute NOP occupies the issue queue (requires `detailed_instruction_tracking=1`) |
| `compute_latency_branch` | `0` | Extra cycles a branch/jump occupies the issue queue (requires `detailed_instruction_tracking=1`) |
| `compute_latency_other` | `0` | Extra cycles an unclassified (OTHER) NOP occupies the issue queue (works on all ISAs) |

### System-mode parameters

| Parameter | Default | Description |
|---|---|---|
| `system_mode` | `0` | If 1, run `qemu-system-*` instead of `qemu-*` user-mode |
| `system_mode_loader` | `-kernel` | Flag inserted before the executable path in system mode (`-kernel` for ELF, `-bios` for raw ROM images) |

### Platform presets

| Parameter | Default | Description |
|---|---|---|
| `platform` | `""` | Built-in preset (`riscv64_virt`, `riscv64_virt_uart`, `arm_m7`, `x86_baremetal`, `*_usermode`). Supplies QEMU defaults and `region_handler` presets when slots are not populated in SDL. |

### Address regions (`region_handler` subcomponents)

Use `setSubComponent("region_handler", ...)` instead of flat `memmap*` params.
First matching handler wins; put specific regions (UART) before broad filters.

| SubComponent | Purpose |
|---|---|
| `quetz.ForwardRegionHandler` | Forward traffic to memHierarchy (optional explicit range) |
| `quetz.FilteredRegionHandler` | Count `filtered_reads` / `filtered_writes`; drop |
| `quetz.UartRegionHandler` | Capture TX bytes at `tx_offset`; drop |
| `quetz.MmioForwardRegionHandler` | Forward MMIO range (optional `mmio_link` port) |

Example (RISC-V virt UART + filtered RAM):

```python
uart = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 0)
uart.addParams({"start": "0x10000000", "end": "0x10000FFF", "tx_offset": "0"})
ram = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 1)
ram.addParams({"start": "0x0", "end": "0x7FFFFFFF"})
```

### Pipeline stage subcomponents (per vCPU)

| Slot | Default class | Role |
|---|---|---|
| `pipeline_input` | `quetz.DefaultPipelineInput` | Drain IPC ring |
| `pipeline_filter` | `quetz.DefaultPipelineFilter` | Region handlers |
| `pipeline_transform` | `quetz.DefaultPipelineTransform` | Stalls / NOP / MemOp |
| `pipeline_output` | `quetz.DefaultPipelineOutput` | Issue `StandardMem` |

Override a stage per vCPU index, e.g. `cpu.setSubComponent("pipeline_output", "quetz.LoggingPipelineOutput", 0)`.

---

## Ports

| Port | Description |
|---|---|
| `cache_link_0` … `cache_link_N` | Per-vCPU connection to the memory hierarchy. `N = vcpu_count - 1` |

Each port should be connected to an L1 cache or directly to a MemController.
The component also supports subcomponent slots: `memory` (per-vCPU StandardMem),
`region_handler`, and the four `pipeline_*` stages (see above).

---

## Statistics

All statistics are per-vCPU and labelled with a vCPU index suffix (e.g.
`cpu.read_requests.0`, `cpu.read_requests.1`, …).

| Statistic | Unit | Description |
|---|---|---|
| `read_requests` | requests | Reads forwarded to the cache |
| `write_requests` | requests | Writes forwarded to the cache |
| `read_latency` | cycles | Cumulative round-trip read latency |
| `write_latency` | cycles | Cumulative round-trip write latency |
| `read_request_sizes` | bytes | Size distribution of read requests |
| `write_request_sizes` | bytes | Size distribution of write requests |
| `split_read_requests` | requests | Extra sub-requests beyond the first for reads that crossed cache-line boundaries (an access spanning N lines contributes N-1 to this counter) |
| `split_write_requests` | requests | Extra sub-requests beyond the first for writes that crossed cache-line boundaries (same N-1 convention) |
| `no_ops` | instructions | Instructions with no memory side-effect |
| `instruction_count` | instructions | Total instructions observed |
| `cycles` | cycles | Simulated clock cycles |
| `active_cycles` | cycles | Cycles with ≥1 memory operation issued |
| `filtered_reads` | requests | Reads to filtered regions (dropped) |
| `filtered_writes` | requests | Writes to filtered regions (dropped) |
| `stall_cycles` | cycles | Stall cycles from the execution latency model |
| `int_compute` | instructions | Non-memory integer ALU instructions (0 unless `detailed_instruction_tracking=1`) |
| `fp_compute` | instructions | Non-memory scalar FP arithmetic instructions (0 unless enabled) |
| `vec_compute` | instructions | Non-memory vector/SIMD arithmetic instructions (0 unless enabled) |
| `branch` | instructions | Branch, jump, call, and return instructions (0 unless enabled) |

---

## Memory hierarchy wiring

### Single-core with L1 cache (recommended)

```python
l1 = sst.Component("l1", "memHierarchy.Cache")
l1.addParams({
    "access_latency_cycles": 2,
    "cache_frequency"      : "1GHz",
    "coherence_protocol"   : "MSI",
    "associativity"        : 4,
    "cache_line_size"      : 64,
    "cache_size"           : "32KB",
    "L1"                   : 1,
})

sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"),
                            (l1,  "highlink",     "1ns"))
sst.Link("l1_mem").connect( (l1,  "lowlink",  "50ns"),
                            (mem, "highlink",  "50ns"))
```

### Multi-level hierarchy (L1 → L2 → memory)

```python
# L2 differs from L1: L1=0, larger size, higher associativity
l2.addParams({"L1": 0, "cache_size": "256KB", "associativity": 8, ...})

sst.Link("cpu_l1").connect((cpu, "cache_link_0", "1ns"), (l1, "highlink", "1ns"))
sst.Link("l1_l2").connect( (l1,  "lowlink",  "5ns"),  (l2, "highlink", "5ns"))
sst.Link("l2_mem").connect((l2,  "lowlink", "50ns"), (mem, "highlink", "50ns"))
```

### Multi-core with shared bus

Each vCPU needs its own private L1 cache; L1 caches share a bus.

```python
cpu.addParams({"vcpu_count": 2, ...})

for i in range(2):
    l1 = sst.Component(f"l1_{i}", "memHierarchy.Cache")
    l1.addParams({"L1": 1, "cache_size": "32KB", ...})
    sst.Link(f"cpu_l1_{i}").connect((cpu, f"cache_link_{i}", "1ns"),
                                     (l1,  "highlink",        "1ns"))
    sst.Link(f"l1_bus_{i}").connect( (l1,  "lowlink",  "5ns"),
                                     (bus, f"highlink{i}", "5ns"))

sst.Link("bus_mem").connect((bus, "lowlink0", "1ns"), (mem, "highlink", "1ns"))
```

---

## RVV (RISC-V Vector Extension)

QEMU user-mode supports RVV out of the box for any binary compiled with
`-march=rv64gcv`.  No extra component parameters are needed.  The plugin
classifies vector loads/stores (`vle*`, `vse*`) via their opcode encoding
and reports them in a separate instruction class so that `exec_latency_vec`
and future per-class statistics can distinguish scalar from vector traffic.

Build a test binary:
```bash
/opt/riscv/bin/riscv64-unknown-linux-musl-gcc \
    -march=rv64gcv -mabi=lp64d -O2 -static \
    my_rvv_kernel.c -o my_rvv_kernel
```

---

## Multi-core / threading

Set `vcpu_count` to the number of OS threads the binary will create.  QEMU
user-mode transparently handles `pthread_create` and OpenMP — each new thread
becomes a new vCPU in the plugin's view.

**Halt quorum:** the simulation ends only when **every** vCPU has halted (via
EXIT or `max_insts`) **and** every vCPU has drained its in-flight memory
transactions.  A single vCPU exiting early does not tear down the simulation
while other vCPUs still have queued work.

Pass `OMP_NUM_THREADS` via `envparamcount`/`envparamname0`/`envparamval0` so
the child process inherits the correct thread count:

```python
cpu.addParams({
    "vcpu_count"   : 4,
    "envparamcount": 1,
    "envparamname0": "OMP_NUM_THREADS",
    "envparamval0" : "4",
})
```

---

## Tests

All tests are in `tests/`.  Run any test with:

```bash
sst tests/<test_name>.py
```

| Test | Binary source | What it exercises |
|---|---|---|
| `test_hello.py` | vanadis `hello-world` | Basic wiring, no cache |
| `test_hello_l1cache.py` | vanadis `hello-world` | L1 cache wiring |
| `test_sqrt_fp.py` | vanadis `sqrt-float` | FP instruction class, `exec_latency_fp` |
| `test_rvv_saxpy.py` | local `rvv_saxpy` | RVV vector instructions, `exec_latency_vec` |
| `test_stream.py` | vanadis `stream` | L1+L2 hierarchy, memory bandwidth |
| `test_multicore.py` | vanadis `openmp` | 2-vCPU simulation, per-core L1 + shared bus |
| `test_merlin_2node.py` | local `rvv_saxpy` | 2 nodes × L1 → Merlin hr_router → shared L2 → memory |
| `test_x86_hello.py` | local `hello_x86_64` | x86-64 via GENERIC ISA classifier (size-based) |
| `test_aarch64_hello.py` | local `hello_aarch64` | AArch64 with per-class instruction classifier |
| `test_mips_hello.py` | vanadis `mipsel/hello-world` | MIPS EL via GENERIC ISA classifier |

The `rvv_saxpy`, `hello_x86_64`, and `hello_aarch64` binaries are included in
the `tests/` directory.  All vanadis binaries are found automatically via
relative path from the `tests/` directory.

The multi-ISA test binaries (`hello_x86_64`, `hello_aarch64`) are compiled from
`tests/hello_multiisa.c` (Leibniz pi approximation with FP arithmetic):

```bash
# x86-64 (native)
gcc -O2 -static -o tests/hello_x86_64 tests/hello_multiisa.c

# AArch64 (cross-compiler required)
aarch64-linux-gnu-gcc -O2 -static -o tests/hello_aarch64 tests/hello_multiisa.c
```