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

## Scope and limits (read before planning a port)

Full detail in [SIMULATING-YOUR-SYSTEM.md](SIMULATING-YOUR-SYSTEM.md)
§ *What works today* / § *Supported parts*; the one-screen version:

- **Functional fidelity, not cycle accuracy.** The guest executes real
  instructions and devices return real data; all timing (memory path,
  device latency, IRQ delivery) is approximate. Assert function, never
  timing.
- **Freestanding firmware or ported app + drivers — not board BSPs.**
  QEMU's `mcf5208evb` reads unmodeled SoC space (PLL, SCM, WDT, GPIO, …)
  as zero and ignores writes, so unmodified BSP init **hangs on status
  polls and silently loses config writes** (it does not crash). Port the
  application and its drivers onto the shipped startup scaffold instead.
- **One ColdFire machine.** `mcf5208evb` is the reference vehicle;
  other family parts map onto it via linker script + deck env (memory
  map, UART base, INTC lines). ColdFire V4 targets are assessed and
  CI-gated — run with `-cpu cfv4e` (`test_quetz_coldfire_v4_fpu`,
  `test_quetz_coldfire_irq_cfv4e`). A different SoC means a new QEMU
  machine — ask first.
- **Interrupts:** QEMU-native device IRQs (UART/timer/FEC) and SST-device
  IRQ injection both work; IRQ *latency* is functional, not modeled.

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
Region handlers act on the **trace** stream (what reaches memHierarchy);
addresses matching no handler are forwarded, so cover QEMU-serviced device
space (e.g. the ColdFire SoC registers) with a `FilteredRegionHandler` or
the forwarded traffic will miss every MemController range.

| SubComponent | Purpose |
|---|---|
| `quetz.ForwardRegionHandler` | Forward traffic to memHierarchy (optional explicit range) |
| `quetz.FilteredRegionHandler` | Count `filtered_reads` / `filtered_writes`; drop |
| `quetz.UartRegionHandler` | Capture TX bytes at `tx_offset` into stdout + stats; drop |
| `quetz.MmioForwardRegionHandler` | Forward the range on `mmio_link_%d` instead of `cache_link_%d` |
| `quetz.GpuTraceRegionHandler` | Count doorbell/status/other accesses in a GPU-shaped range (`doorbell_offset`, `status_offset`); trace-only |
| `quetz.TestFinisherRegionHandler` | End the simulation when the guest stores a sentinel (`0x5555` PASS / `0x3333` FAIL) at `start` |

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
| `mmio_link_0` … `mmio_link_N` | Per-vCPU MMIO path (optional): traffic matched by an `MmioForwardRegionHandler` and synchronous-MMIO requests go here instead of `cache_link` |
| `irq_link_0`, `irq_link_1`, … | IRQ-injection links from SST MMIO devices (one per device, contiguous from 0); see § IRQ injection |

Each `cache_link` should be connected to an L1 cache or directly to a
MemController.  The component also supports subcomponent slots: `memory` /
`mmio` (per-vCPU StandardMem), `region_handler`, `accelerator`, and the four
`pipeline_*` stages (see above).

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
| `compute_stall_cycles` | cycles | Stall cycles from the compute latency model |
| `int_compute` | instructions | Non-memory integer ALU instructions (0 unless `detailed_instruction_tracking=1`) |
| `fp_compute` | instructions | Non-memory scalar FP arithmetic instructions (0 unless enabled) |
| `vec_compute` | instructions | Non-memory vector/SIMD arithmetic instructions (0 unless enabled) |
| `branch` | instructions | Branch, jump, call, and return instructions (0 unless enabled) |
| `mmio_read_requests` / `mmio_write_requests` | requests | Traffic forwarded on `mmio_link` instead of `cache_link` |
| `mmio_read_latency` / `mmio_write_latency` | cycles | Round-trip latency of `mmio_link` traffic |
| `mmio_truncated_writes` / `cached_truncated_writes` | requests | Store payloads wider than the IPC data cap (trailing bytes not carried) |
| `mmio_doorbell_flushes` / `mmio_doorbell_flush_cycles` | requests / cycles | Coherence flushes issued before accelerator doorbells (§ Accelerator ports) |
| `async_submits` / `async_completions` | requests | Posted offloads submitted / retired via the async aperture |
| `async_overlap_cycles` | cycles | Cycles a vCPU advanced while its posted offload was in flight |
| `gpu_doorbell_writes` / `gpu_status_polls` / `gpu_other_reads` / `gpu_other_writes` | requests | `GpuTraceRegionHandler` observation counters |

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

## System mode and synchronous MMIO

In **system mode** (`system_mode=1`) QuetzCPU runs `qemu-system-<arch>` with
your machine flags in `qemu_args` and the firmware ELF as `-kernel` (or
`-bios` via `system_mode_loader`).  Guest RAM lives in QEMU; SST sees the
instruction/memory *trace* through the plugin, routed by the region
handlers.  Two additional mechanisms make guest⇄SST interaction real rather
than trace-only:

**Synchronous MMIO window (`sst-mmio-bridge`).**  When the environment has
`QUETZ_MMIO_PAYLOAD=1` and `QUETZ_MMIO_START`/`QUETZ_MMIO_END`, the launcher
adds a bridge device to the QEMU command line covering that range.  A guest
load/store in the window *blocks the vCPU* until SST answers: the request
crosses a shared-memory mailbox, QuetzCPU forwards it as a
`StandardMem::Read`/`Write` on `mmio_link_%d`, and the response value is
what the guest register sees.  This is how the MMIO device components below
are reached.  In **user mode** the same env vars reserve the aperture
PROT_NONE and route the resulting SIGSEGV to the same mailbox — decks and
firmware are identical either way.

**SST-backed memory window.**  `QUETZ_SST_WIN_START`/`QUETZ_SST_WIN_END`
map a second bridge aperture whose contents live in the SST memory
hierarchy (directory + MemController), not QEMU RAM.  Use it when a device
DMA and the guest must see the same bytes — e.g. kernel-compute buffers
(ARG0/ARG1 point into the window).  Because the mailbox carries *values*
and SST serializes them little-endian, big-endian guests read/write the
window with plain word accesses and no byte swapping.

---

## MMIO device components

Three shipped SST-side devices implement guest-visible register files.  In
system mode they sit behind the synchronous-MMIO window (see § System mode);
the guest reads/writes their registers like any memory-mapped peripheral and
the SST component's answer is what the guest sees.  All three follow the
same conventions: 8-byte register stride, values ≤32 bits so 32-bit guests
read them whole, and *numeric* byte packing (`b0 | b1<<8 | b2<<16 | b3<<24`)
so identical firmware is correct on big-endian (ColdFire) and little-endian
(RISC-V) guests.  Each register map below is authoritative in the named
header.

### `quetz.QuetzGpuDevice` — generic accelerator

A doorbell/status accelerator.  With the `kernel` slot **empty** it is a
pure latency model: a doorbell write starts a BUSY window
(`kernel_latency` cycles, or the value last written to
`REG_LATENCY_OVERRIDE`), STATUS shows busy/idle, KERNEL_ID counts
completions.  With a `kernel` subcomponent loaded the device really
computes: DMA-read the input from `REG_ARG0`, run the kernel, hold BUSY for
the kernel's modeled latency, DMA-write the result to `REG_ARG1`, and only
then release the (blocking) doorbell.

Register map (`quetz_gpu_device.h`):

| offset | reg | dir | behavior |
|-------:|-----|-----|----------|
| 0x00 | DOORBELL | W | submit an op (blocking when `doorbell_blocking=1`) |
| 0x08 | STATUS | R | 1 = busy (kernel ops: busy for the whole doorbell-to-writeback lifetime) |
| 0x10 | KERNEL_ID | R | completed-op counter |
| 0x18 | LATENCY_OVERRIDE | W | cycles for the *next* op (0 = use default/kernel opinion) |
| 0x20 | TICKET | R | last submit ticket (async poll contract) |
| 0x28 | RESULT | R | completed-op latch (mirrors KERNEL_ID on the synthetic device) |
| 0x30–0x48 | ARG0–ARG3 | W | kernel operands: ARG0 = input addr, ARG1 = output addr, ARG2/ARG3 kernel-defined |
| 0x50 | IRQ_ACK | R/W | R: completion line raised; W nonzero: ack (lower the line) |

Key parameters: `base_addr`, `mmio_size`, `kernel_latency`,
`doorbell_blocking` (required =1 when a kernel is loaded), `irq_line` /
`irq_vcpu` (completion IRQ, −1 = disabled).  Slots: `iface` (MMIO target,
`memHierarchy.standardInterface`), `mem_iface` (memory initiator for kernel
DMA), `kernel` (`SST::Quetz::QuetzKernel`).  Port: `irq` (see § IRQ
injection).  Stats: `kernels_launched`, `busy_cycles`, `doorbell_writes`,
`status_polls`, `latency_overrides`, `doorbell_while_busy`, `irqs_raised`,
`wrong_direction_accesses`, `bad_offset_accesses`.

Shipped kernels for the `kernel` slot (write your own by subclassing
`QuetzKernel` — two methods, `quetz_kernel_api.h`; tutorial in
SIMULATING-YOUR-SYSTEM.md § Adding your own kernel):

| kernel | data | ARG2 | ARG3 | latency param |
|---|---|---|---|---|
| `quetz.FFTKernel` | LE float32 cfloat[N], radix-2 | N | — | `fft_latency_coeff` (default 20) × N·log₂N |
| `quetz.ScaleOffsetKernel` | LE s16[N], sat16(s·scale+offset) | N | scale \| offset<<16 | `latency_coeff` (default 4) × N |

### `quetz.QuetzStreamDevice` — recorded-data feed (stimulus in)

Replays a binary fixture file (`stream_file`) through a FIFO register
interface — sensors, telemetry, CAN logs, any recorded byte stream.
Optionally paced: with `pace_bytes`/`pace_period` set, STATUS fills over
simulated time and firmware polling/timeout logic gets exercised; a paced
refill that makes STATUS go 0 → nonzero can raise a data-ready IRQ
(`irq_line`).

Register map (`quetz_stream_device.h`):

| offset | reg | dir | behavior |
|-------:|-----|-----|----------|
| 0x00 | STATUS | R | bytes ready now (unpaced: all remaining) |
| 0x08 | DATA | R | pop up to 4 bytes, numerically packed |
| 0x10 | SEQ | R | bytes consumed so far |
| 0x18 | CTRL | W | 1 = rewind (paced: restarts the refill budget) |
| 0x20 | EOS | R | 1 = stream fully consumed |
| 0x28 | IRQ_ACK | R/W | R: data-ready line raised; W nonzero: ack |

Stats: `data_reads`, `bytes_delivered`, `status_polls`, `underruns`,
`not_ready_reads`, `paced_refills`, `rewinds`, `irqs_raised`,
`wrong_direction_accesses`, `bad_offset_accesses`.

### `quetz.QuetzSinkDevice` — capture file (response out)

The write-side mirror of the stream device: the guest pushes bytes and SST
captures them to `sink_file` for host-side assertion (byte-exact diff
against a golden or computed expectation — `assert_sink_equals` in
`tests/quetz_test_helpers.py`).  A DATA write pushes exactly the store's
width (8/16/32-bit store → 1/2/4 bytes, value unpacked low-byte-first), so
trailing partial words need no extra register.  The file is written on CTRL
flush and unconditionally at simulation end; `max_bytes` caps runaway
captures (excess dropped + counted).

Register map (`quetz_sink_device.h`):

| offset | reg | dir | behavior |
|-------:|-----|-----|----------|
| 0x00 | STATUS | R | bytes accepted so far (== SEQ) |
| 0x08 | DATA | W | push write-size bytes |
| 0x10 | SEQ | R | bytes accepted so far |
| 0x18 | CTRL | W | 1 = flush to file now; 2 = truncate/restart capture |

Stats: `bytes_accepted`, `flushes`, `truncates`, `dropped_bytes`,
`wrong_direction_accesses`, `bad_offset_accesses`.

### Wiring devices into one window

Multiple devices share one bridge window, routed by address.  Two shipped
patterns:

- **Bus** (simple, the system demo): CPU `mmio_link_0` → `memHierarchy.Bus`
  `highlink0`; each device's `iface` on a `lowlink%d` with a disjoint
  `base_addr` inside the window — `tests/sysmode/basic_quetz_coldfire_system.py`.
- **NoC** (when a device also initiates memory traffic, e.g. kernel DMA):
  every interface gets a `memHierarchy.MemNIC` on a `merlin.hr_router`;
  MMIO targets advertise their address region, initiators route by range —
  `tests/sysmode/basic_quetz_gpu_compute_coldfire.py`.

To write your own device, copy `quetz_stream_device.{h,cc}` (read-side) or
`quetz_sink_device.{h,cc}` (write-side); the recipe is in
SIMULATING-YOUR-SYSTEM.md § Adding your own device.

---

## IRQ injection (SST devices → guest interrupts)

QEMU-native device IRQs (UART, timers, FEC) vector normally with no quetz
involvement.  SST-side devices raise guest interrupts through a reverse
shared-memory mailbox (`quetz_ipc_types.h`: one seqlock slot per vCPU row ×
machine line) that the patched QEMU bridge polls on a virtual-time timer
and applies to the machine's interrupt controller.  Level semantics: a
device raises its line on the event of interest and holds it until the
guest writes the device's `IRQ_ACK` register.  Latency is functional
(~`QUETZ_IRQ_POLL_NS`, default 10 µs of virtual time) — never assert
timing against it.

Wiring (worked example: `tests/sysmode/basic_quetz_coldfire_system.py` with
`QUETZ_GPU_IRQ_LINE`/`QUETZ_SENSOR_IRQ_LINE`; guest side:
`tests/sysmode/firmware/coldfire_intc.h` + `coldfire_irq_demo.c`):

1. device param `irq_line` = interrupt-controller input number
   (mcf5208evb: 29–35 are free; UARTs own 26–28, PITs 4–5, FEC 36+);
2. an SST Link from the device's `irq` port to a CPU `irq_link_%d` port
   (a `quetz.QuetzIrqEvent{vcpu, line, level}` per level change);
3. `QUETZ_IRQ_LINES=<n>` in the environment so the launcher enables the
   bridge's poll (`intc-type` defaults to `mcf-intc`; override with
   `QUETZ_IRQ_INTC_TYPE` for another controller exposing qdev GPIO inputs).

---

## Accelerator ports (balar and other doorbell accelerators)

The `accelerator` subcomponent slot holds host-side ports that own a
doorbell aperture inside the synchronous-MMIO window and implement the
policy a real accelerator needs beyond a plain register file.
`quetz.BalarAcceleratorPort` (the shipped implementation, driving balar →
GPGPU-Sim) provides:

- **Coherence flush before doorbell** (`flush_mode=range_from_value`):
  a doorbell write carries the staged-packet address; the port
  `FlushAddr(inv)`s `packet_flush_bytes` from it on the cache link before
  forwarding the doorbell on the mmio link, so the accelerator reads what
  the CPU wrote.
- **Posted/async offload** (`async_offload=1`): a separate aperture
  (`async_doorbell_addr`, registers SUBMIT/TICKET/COMPLETED/STATUS/RESULT)
  returns a ticket immediately and completes in the background,
  up to `async_completion_depth` in flight per vCPU — the guest overlaps
  CPU work with the accelerator and polls the completion counter.

Back-compat: setting the CPU-level `balar_doorbell_addr`/`async_*` params
with an empty `accelerator` slot auto-creates a default port with those
values.  The synthetic `QuetzGpuDevice` needs no port — its registers are
plain synchronous MMIO; use a port when the accelerator sits behind
staged-packet coherence or posted completion.

---

## Environment variables

The component contract is its SST params; these env vars configure the
*launcher* (read at QEMU spawn time, same process as the deck):

| Variable | Effect |
|---|---|
| `QUETZ_MMIO_PAYLOAD=1` | Enable the synchronous-MMIO aperture (bridge device / user-mode trap) |
| `QUETZ_MMIO_START` / `QUETZ_MMIO_END` | Guest-physical range of the MMIO window |
| `QUETZ_SST_WIN_START` / `QUETZ_SST_WIN_END` | Optional SST-backed memory window (system mode only) |
| `QUETZ_IRQ_LINES` | >0 enables bridge IRQ polling for lines [0, n) |
| `QUETZ_IRQ_POLL_NS` | Bridge IRQ poll period in virtual-time ns (default 10000) |
| `QUETZ_IRQ_INTC_TYPE` | QOM type of the interrupt controller (default `mcf-intc`) |

Everything else `QUETZ_*` (e.g. `QUETZ_EXE`, `QUETZ_QEMU`,
`QUETZ_SENSOR_FILE`, `QUETZ_SINK_FILE`, `QUETZ_GPU_IRQ_LINE`,
`QUETZ_SENSOR_PACE_BYTES`, `QUETZ_STATS_OUT`, …) is a **deck convention**,
not a component contract: the shipped decks read them to build the SST
graph, and each deck's docstring lists the ones it honors.  The test
helpers (`make_sysmode_env`, `enable_mmio_payload_delivery`) manage them
for the testsuite.

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

Basic hello-world wiring across ISAs (riscv64 with/without L1, x86-64, aarch64)
is covered by the parametrized matrix in `testsuite_default_quetz.py`
(`build_quetz_test_matrix`), which drives the shared `usermode/basic_quetz.py`
deck. The standalone decks below exercise scenarios not in that matrix:

| Test | Binary source | What it exercises |
|---|---|---|
| `test_sqrt_fp.py` | vanadis `sqrt-float` | FP instruction class, `exec_latency_fp` |
| `test_rvv_saxpy.py` | local `rvv_saxpy` | RVV vector instructions, `exec_latency_vec` |
| `test_stream.py` | vanadis `stream` | L1+L2 hierarchy, memory bandwidth |
| `test_multicore.py` | vanadis `openmp` | 2-vCPU simulation, per-core L1 + shared bus |
| `test_merlin_2node.py` | local `rvv_saxpy` | 2 nodes × L1 → Merlin hr_router → shared L2 → memory |
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