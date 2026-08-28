# Quetz (Quetzalcoatl) — Technical Outline

This document describes the **quetz** SST element: what it is for, how it is implemented, how UART and MMIO interact with it, driver-level context, and recommendations for making it a stronger platform for QEMU-based bare-metal simulation.

---

## 1. Goal of Quetz

### 1.1 Problem statement

Many architecture and memory-system studies need to run **real binaries** (compiled for RISC-V, AArch64, x86, etc.) through a **detailed memory hierarchy** (L1/L2/DRAM, coherence, NoC) without building a full cycle-accurate CPU model. Traditional options:

| Approach | Strength | Weakness |
|----------|----------|----------|
| **Trace replay** | Deterministic, fast | Traces are huge; hard to vary ISA/binary; no easy “run this ELF” workflow |
| **Full CPU model** (e.g. Vanadis) | Structural fidelity | Heavy to build/maintain; lag behind production ISAs (RVV, SVE, etc.) |
| **Pin + Ariel** | Mature SST integration | Pin is x86-centric; licensing/portability limits |
| **QEMU alone** | Runs anything QEMU supports | No SST timing for caches/memory; functional-only for hierarchy studies |

**Quetz’s goal** is to occupy the middle ground: use **QEMU** (user-mode or system-mode) as a **functional front-end** that executes guest code, while SST’s **memHierarchy** models **timing and capacity** of caches and memory. The guest’s memory traffic is observed at instruction granularity and replayed into `StandardMem` as read/write requests.

### 1.2 What Quetz is *not*

- Not a cycle-accurate pipeline (no fetch/decode/issue queues, branch prediction, etc.).
- Not a replacement for QEMU’s device models in system mode (UART, timers, disks are still mostly QEMU’s job unless explicitly filtered or side-captured).
- Not a complete SoC simulator: there is no first-class device bus, interrupt controller, or DMA engine in Quetz itself.

### 1.3 Primary use cases

1. **User-mode studies** — Linux/static binaries under `qemu-riscv64`, `qemu-aarch64`, `qemu-x86_64`: cache sensitivity, bandwidth (`stream`), FP/vector latency knobs, multi-threaded OpenMP with per-vCPU L1s.
2. **Bare-metal / firmware studies (system mode)** — Small kernels linked for `qemu-system-*` (`-kernel`, `-machine virt`, etc.): measure DRAM traffic for memcpy/compute while **excluding** MMIO and platform RAM from cache statistics via `memmap` regions.
3. **ISA extension exploration** — RISC-V Vector (RVV) and similar: QEMU executes extended instructions; Quetz classifies vector mem ops separately for `exec_latency_vec` and statistics.

### 1.4 Design lineage

Quetz deliberately **mirrors Ariel’s split**:

- Parent SST process owns simulation time and memory hierarchy.
- Child process runs the “CPU” (QEMU instead of Pin).
- **POSIX shared memory** + per-thread ring buffers (`TunnelDef`) carry commands/events.

The README states this explicitly: same IPC pattern as `ariel/ariel_shmem.h`, but the child is QEMU + a TCG plugin instead of an instrumented binary under Pin.

---

## 2. Implementation Details

### 2.1 End-to-end data flow

```
Guest binary / firmware
        │
        ▼
┌───────────────────┐     TB translate + run-time hooks
│  QEMU (child)     │     libqemu_sst_plugin.so
│  - user: qemu-*   │──────────────────────────────┐
│  - system:        │                              │
│    qemu-system-*  │                              ▼
└───────────────────┘              Per-vCPU circular buffer (QuetzCommand)
        │                                        │
        │  fork/exec from                        │  POSIX shm (QuetzTunnel)
        ▼                                        ▼
┌───────────────────────────────────────────────────────────────┐
│  SST parent: QuetzCPU                                         │
│    tick() → QuetzCore[v]::refillQueue() / processQueue()      │
│      READ/WRITE → StandardMem → memHierarchy (Cache / MC)     │
│      NOP → stats + optional compute_latency stall             │
│      EXIT → halt vCPU; quorum ends sim when all drained       │
└───────────────────────────────────────────────────────────────┘
```

**Back-pressure:** Ring buffers are bounded (`maxcorequeue`). When SST falls behind, the plugin’s `writeMessage` blocks (spins), which naturally throttles QEMU—similar in spirit to Ariel stalling Pin when queues fill.

### 2.2 File-by-file responsibilities

The SST side and QEMU plugin are split into small translation units (see §2.5 and Appendix B). Each file below maps to one primary concern.

#### `quetz_shmem.h` (shared protocol — compiled into **both** SST lib and QEMU plugin)

| Piece | Role |
|-------|------|
| `QuetzShmemCmd` | `NOP`, `READ`, `WRITE`, `EXIT` |
| `QuetzInsnClass` | 8 classes: 3 memory (INT/FP/VEC), 4 compute, branch, other |
| `QuetzCommand` | One IPC record: cmd, size, pc, addr, insn_class, `data[16]` for store payload |
| `QuetzSharedData` | `numCores`, `simTime`, `simCycles`, `child_attached` |
| `QuetzTunnel` | `TunnelDef` facade composing `QuetzCommandBuffer`, `QuetzSyncManager`, `QuetzStatisticsCollector` |
| `quetz_ipc_types.h` | Wire types only (`QuetzCommand`, enums, `QuetzSharedData`) |
| `QuetzCommandBuffer` | Per-vCPU ring buffer read/write via `TunnelDef` |
| `QuetzSyncManager` | `child_attached` handshake, `waitForChild()` |
| `QuetzStatisticsCollector` | SHM `simTime`/`simCycles`; SST `record*` facade over `QuetzCoreStats` |

**Core functionality:** Defines the **only** contract between processes. Header-only templates from `sst/core/interprocess/tunneldef.h` avoid linking the plugin against `libsst`.

**Refactor suggestions:**

- Move to a **standalone `quetz_ipc.h`** with zero SST includes on the plugin path (today the plugin still includes SST interprocess headers).
- Version the protocol (`QUETZ_IPC_VERSION`) for forward compatibility.
- Extend `data[]` or add scatter/gather for wide vector stores (today capped at 16 bytes in the command).

---

#### `quetzcpu.h` / `quetzcpu.cc` (~170 lines — SST `Component` lifecycle)

| Responsibility | Details |
|----------------|---------|
| ELI registration | Parameters, ports `cache_link_0..N`, statistics, `memory` subcomponent slot |
| Orchestration | `QuetzConfig::fromParams()`, tunnel/clock setup, per-vCPU `QuetzCore` + `StandardMem` wiring |
| SHM parent | `SHMParent<QuetzTunnel>` creates region; `waitForChild()` in `init(0)` |
| Process launch | Delegates to `QemuLauncher::spawn()` (see `quetz_launcher`) |
| Clock | Registers `tick()`; updates tunnel time/cycles each cycle |
| Shutdown | **Halt quorum:** all cores `halted_` AND `pending_count_ == 0` before `primaryComponentOKToEndSim()` |
| Cleanup | `finish()` / `emergencyShutdown()` → `QemuLauncher::terminate()` / `forceKill()` |

Configuration parsing and QEMU fork/exec live in separate units (below); `quetzcpu` stays the SST entry point only.

---

#### `quetz_config.h` / `quetz_config.cc` (`QuetzConfig` — parameter parsing)

| Responsibility | Details |
|----------------|---------|
| Params | All ELI knobs: QEMU paths, `system_mode`, memmap regions, latency tables, env/stdio, ISA metadata |
| Validation | Fatal if `compute_latency_{int,fp,vec,branch}` set without `detailed_instruction_tracking=1` |
| Output | Populates `QuetzConfig` struct consumed by `QuetzCPU` and `QemuLauncher` |

---

#### `quetz_launcher.h` / `quetz_launcher.cc` (`QemuLauncher` — child process)

| Responsibility | Details |
|----------------|---------|
| `spawn()` | Resolve `qemu_plugin` path, build argv, `fork()` + `execvp()` |
| argv layout | User mode: `qemu-<arch> [qemu_args] -plugin <plugin,shmname=...> <executable> [appargs]` |
| System mode | Inserts `system_mode_loader` (`-kernel` or `-bios`) before executable |
| Plugin args | `shmname=<region>`; optional `detailed=1` for per-class NOP tagging |
| Child setup | `prctl(PR_SET_PTRACER)`, `setenv` for extra env, `freopen` for stdin/stdout/stderr |
| Signals | `terminate()` (SIGTERM), `forceKill()` (SIGKILL) |

---

#### `quetzcore.h` / `quetzcore.cc` (~240 lines — per-vCPU event pump)

| Responsibility | Details |
|----------------|---------|
| `refillQueue()` | Non-blocking drain from tunnel → `coreQ_` (staged with stall countdown) |
| `processQueue()` | Dispatch commands; delegate mem issue / filtering to helper classes |
| Latency model | `exec_latency_*` on READ/WRITE at queue head; `compute_latency_*` on NOP |
| Delegation | `MemMap` (regions + UART), `MemRequestEmitter` (StandardMem), `QuetzCoreStats` |

**Important behaviors:**

- Filtered/UART ops **do not** consume issue bandwidth or pending slots (correct for MMIO).
- Wide accesses (e.g. RVV) split into multiple `StandardMem` requests; only first 16 bytes of write data forwarded.
- `max_insts` halts core after instruction count (includes NOPs and filtered ops).

**Done:** per-region policy is a `MemRegionHandler` subcomponent; `MmioForwardRegionHandler` returns `Action::FORWARD_MMIO` so traffic uses per-vCPU `mmio_link_N` on `QuetzCPU` (not the handler subcomponent).

---

#### `quetz_region_handler.h`, `quetz_region_handlers.*`, `quetz_region_table.*`

| Responsibility | Details |
|----------------|---------|
| API | `MemRegionHandler` SubComponent (`FORWARD` / `FORWARD_MMIO` / `CONSUME`) |
| Built-ins | `ForwardRegionHandler`, `FilteredRegionHandler`, `UartRegionHandler`, `MmioForwardRegionHandler` |
| Lookup | `MemRegionTable::findHandler()` — first-match over populated `region_handler` slots |

---

#### `quetz_mem_issue.h` / `quetz_mem_issue.cc` (`MemRequestEmitter` — hierarchy traffic)

| Responsibility | Details |
|----------------|---------|
| Issue | `issueRead()` / `issueWrite()` with cache-line splitting |
| Slots | `slotsNeeded()` for `maxissuepercycle` / `maxtranscore` accounting |
| Pending | Tracks in-flight `StandardMem` requests; `handleResponse()` for latency stats |

---

#### `quetz_stats.h` / `quetz_stats.cc` (`QuetzCoreStats` — per-vCPU counters)

| Responsibility | Details |
|----------------|---------|
| Bundle | All 20 `Statistic<uint64_t>*` handles in one struct |
| Registration | `registerAll(QuetzCore*, sub_id)` — `QuetzCore` is a `friend` for protected `registerStatistic` |

---

#### QEMU plugin (`qemu_plugin/` — still builds as `libqemu_sst_plugin.so`)

| File | Role |
|------|------|
| `plugin_main.cpp` | `qemu_plugin_install`, `cb_atexit`, ISA detection, SHM attach, callback registration |
| `plugin_state.h/cpp` | Globals (`g_tunnel`, `g_isa`, …), `write_cmd()` |
| `instrument.h/cpp` | `cb_tb_trans`, mem/exec callbacks, precise vs delayed registration |
| `decoder_riscv.h` | `classify_riscv_insn()`, `classify_rvc_rv64()` (inline) |
| `decoder_aarch64.h` | `classify_aarch64_insn()` (inline) |
| `decoder_generic.h` | `classify_by_size()` (inline) |

| Phase | Function |
|-------|----------|
| Install | Parse `shmname=`, `detailed=`; detect ISA from `info->target_name`; attach `SHMChild` |
| TB translate (`cb_tb_trans` in `instrument.cpp`) | Classify encoding → register QEMU callbacks |
| Memory (`handle_mem`) | Emit READ/WRITE; store data via QEMU 9+ `qemu_plugin_mem_get_value` when available |
| Exec (two flavours) | See §2.3 |
| Exit (`cb_atexit` in `plugin_main.cpp`) | Broadcast `QUETZ_CMD_EXIT` to all vCPUs |

**Testing:** Decoders are pure functions of `uint32_t enc` — unit-test without QEMU.

**Adding a new ISA:** Add `decoder_<isa>.h`, extend ISA enum in `plugin_state.h`, target-name check in `plugin_main.cpp`, and a branch in `instrument.cpp` `cb_tb_trans`.

**Fragility:** **Precise callback mode** (RISC-V/AArch64) registers **only** a mem callback if the classifier says “memory.” Any misclassified opcode **silently drops** memory events. Alternatives: always register mem callbacks (delayed pattern) at the cost of one-instruction NOP bias.

---

#### `Makefile.am` / `configure.m4`

| File | Role |
|------|------|
| `Makefile.am` | Builds `libquetz.la` + `libqemu_sst_plugin.la`; `-DQEMU_PLUGIN_INSTALL_DIR`; installs plugin to `libexec` |
| `configure.m4` | Finds `qemu-plugin.h`, `glib-2.0`, optional `qemu-riscv64`; enables element |

**Refactor:** Optional configure check for `qemu-plugin` support (`-plugin` on help output).

---

#### Tests and SDL (not core runtime, but architecturally important)

| Path | Role |
|------|------|
| `tests/usermode/basic_quetz.py` | Parameterized SDL; env vars from harness |
| `tests/sysmode/basic_quetz_sysmode.py` | System mode + memmap + stdin/stdout capture |
| `tests/testsuite_default_quetz.py` | Matrices, gold comparison, `QuetzStatsFilter` |
| `tests/sysmode/firmware/*.c` | Bare-metal hello, UART echo, ARM M7, x86 multiboot |
| `tests/binaries/` | Prebuilt or locally built guest ELFs |

**Harness note:** Gold files compare **event-count** stats only; timing and `instruction_count`/`no_ops` are filtered as non-deterministic across QEMU versions/environments.

---

### 2.3 Plugin callback models (critical design fork)

**PRECISE (RISC-V, AArch64):** One callback per instruction.

- Memory insn → mem callback only → READ/WRITE
- Non-memory → exec callback only → NOP with class

**DELAYED (GENERIC — x86, MIPS, etc.):** Both mem and exec callbacks; `g_mem_seen` + `g_prev_cls` implement one-instruction-delayed NOP emission.

| Model | Pros | Cons |
|-------|------|------|
| Precise | Clean first/last insn; no phantom NOP at start | **Classifier must be complete** or mem ops vanish |
| Delayed | QEMU decides memory vs not — robust | One-instruction skew; more callbacks per insn |

**Recommendation:** Hybrid for RISC-V/AArch64: always register mem callback; use classifier only for NOP class and mem class tagging (see prior investigation in development).

---

### 2.4 SST ↔ memHierarchy integration

Each `QuetzCore` holds a `StandardMem*` (port `cache_link_N` or `memory` subcomponent). Requests:

- `StandardMem::Read(addr, size, flags, id)`
- `StandardMem::Write(addr, size, data, posted, flags, id)`

Responses drive `read_latency` / `write_latency` statistics. No TLB/page-table modeling in Quetz—QEMU has already translated guest VA to the address space QEMU presents to the plugin (user-mode often 1:1 with host layout for mem callbacks; system mode uses guest physical or virtual per QEMU internals).

**MMU element:** `memHierarchy`’s `standardInterface` can include `mmu/tlb.h` for full-system SST CPUs; Quetz does **not** wire TLB subcomponents today.

---

### 2.5 Module layout (SST + plugin)

```
SST (libquetz.la)                    QEMU plugin (libqemu_sst_plugin.so)
─────────────────                    ─────────────────────────────────────
quetzcpu.{h,cc}     lifecycle        plugin_main.cpp      install, atexit
quetz_config.{h,cc} params           plugin_state.{h,cpp} globals, write_cmd
quetz_launcher.{h,cc} fork/exec      instrument.{h,cpp}   TB/mem/exec hooks
quetzcore.{h,cc}    event pump       decoder_riscv.h      RISC-V classifier
quetz_region_*.h/cc region handlers   decoder_aarch64.h    AArch64 classifier
quetz_pipeline_api.h stage APIs      qemu_plugin/registry.h  plugin registries
quetz_mem_issue.{h,cc} StandardMem   decoder_generic.h    size fallback
quetz_stats.{h,cc}  statistics
quetz_shmem.h       IPC (both sides)
```

**Still shared / unchanged:** `quetz_shmem.h` (plugin still uses SST `tunneldef.h` headers).

**Possible follow-ups (not done):** versioned standalone IPC header; scatter/gather for wide stores; unit tests per decoder; configure check for QEMU `-plugin` support.

---

## 3. UART — General and in Quetz

### 3.1 UART in general (bare-metal context)

A **UART** (Universal Asynchronous Receiver-Transmitter) is a simple serial device. Software sees a **register file**:

| Typical register | Offset | Direction | Purpose |
|------------------|--------|-----------|---------|
| RBR / THR | 0x00 | R / W | Receive buffer / transmit holding (often same address) |
| IER | 0x01 | RW | Interrupt enable |
| FCR | 0x02 | W | FIFO control |
| LCR | 0x03 | RW | Line control (word length, parity) |
| LSR | 0x05 | R | Line status (TX empty, RX ready) |
| … | | | |

**Polling driver pattern** (used in Quetz firmware tests):

```c
while (!(UART_LSR & TX_EMPTY)) { /* spin */ }
UART_THR = byte;
```

**Memory-mapped UART** (RISC-V `virt`, ARM CMSDK): CPU uses load/store to physical addresses (e.g. `0x10000000`).

**I/O-port UART** (x86 COM1 at `0x3F8`): CPU uses `in`/`out` instructions — **not** visible as normal loads/stores in the guest memory map.

QEMU emulates UART hardware internally; in `-nographic` mode, UART0 is often tied to **host stdio** (stdin/stdout).

### 3.2 How UART works in Quetz today

Quetz does **not** implement a UART device model. It has two **orthogonal** mechanisms:

#### A. QEMU / harness path (functional I/O)

| Mechanism | Where | What happens |
|-----------|-------|--------------|
| `appstdin` / `appstdout` | `quetz_launcher.cc` child | `freopen()` redirects QEMU process stdio — used by `uart_echo` test to inject `ABCDE` and capture output |
| QEMU `-serial file:...` | `qemu_args` (x86 test) | Guest port-I/O UART handled by QEMU; SST never sees writes |
| QEMU virt UART | Machine model | MMIO to `0x10000000` handled by QEMU device; plugin may still **see** guest loads/stores to that PA |

#### B. Quetz memmap `type=uart` (observation only)

Configured via:

```python
memmap0_start = 0x10000000
memmap0_end   = 0x1000000F
memmap0_type  = "uart"
memmap0_uart_tx_offset = 0   # THR at base+0
```

In `quetzcore.cc` `processQueue()` via `MemMap`:

1. WRITE to address in UART region → **not** sent to memHierarchy
2. If offset matches `uart_tx_offset` and `size >= 1`, `MemMap::captureUartByte()`
3. At `finishCore()`, `MemMap::flushUart()` prints `UART[N]: <string>`

**Requirements for capture:**

- Plugin must emit WRITE with store data → needs **QEMU 9+** `qemu_plugin_mem_get_value` in system mode
- Guest must use **MMIO stores**, not x86 `out` instructions

**Sysmode `uart_echo` test setup** (`testsuite_default_quetz.py`):

- `sub_ram` region `0x0–0x7FFFFFFF` marked **`filtered`** — most firmware DRAM traffic excluded from cache stats
- UART at `0x10000000` is **not** in a separate uart region in the matrix; echo test relies on **filtered** covering everything except what QEMU handles internally — gold expects `filtered_reads=18`, `filtered_writes=7` (MMIO status-register polling)

### 3.3 Shortcomings

| Issue | Impact |
|-------|--------|
| **No RX model in SST** | Cannot simulate UART input timing or back-pressure in memHierarchy |
| **Single-byte, offset-0 assumption** | Only first byte of write at `uart_tx_offset`; no FIFO burst, 16-bit regs, or STRW to THR+shadow |
| **No LSR read filtering semantics** | Reads to LSR still counted as `filtered_reads` if in filtered region — poll loops inflate stats |
| **x86 port I/O invisible** | `x86_hello.c` explicitly documents plugin cannot see `outb` — must use `-serial file:` |
| **Duplicate output paths** | Bytes go to QEMU stdout **and** optionally `uart_tx_buf_` if region typed uart — confusing for tests |
| **Print only at end** | No per-cycle or streaming SST output for UART (unlike real serial timing) |

### 3.4 Potential improvements

1. **`UartDevice` SST component** — `StandardMem` slave at `base_addr`; implements THR/LSR behavior; generates ReadResp with synthetic LSR flags; connects to Quetz via memmap type `memory` removed in favor of a real link to device.
2. **Register-aware decode table** — map region+offset → REG_THR vs REG_LSR vs ignore for stats.
3. **stdin injection with timing** — model RX IRQ or periodic “byte available” instead of instant QEMU stdin fill.
4. **Plugin I/O port hooks** — if QEMU exposes IN/OUT instrumentation, capture x86 UART (large effort).
5. **Separate gold for UART** — compare `QUETZ_STDOUT_FILE` content for echo tests, not just `filtered_*` counts.

---

## 4. MMIO — Concept, memHierarchy, and Quetz

### 4.1 What MMIO is

**Memory-mapped I/O (MMIO)** maps device registers into the **physical address space**. CPU `load`/`store` instructions access devices as if they were memory (unlike **port I/O** on x86).

Examples in Quetz tests:

| Platform | Device | Address | Access |
|----------|--------|---------|--------|
| RISC-V virt | NS16550 UART | `0x10000000` | MMIO byte |
| RISC-V virt | Test finisher | `0x100000` | MMIO word |
| ARM MPS2 | CMSDK UART | `0x40004000` | MMIO word-wide regs |
| x86 PC | COM1 | `0x3F8` | **Port I/O**, not MMIO |

### 4.2 MMIO in memHierarchy

memHierarchy provides a **reference pattern** for MMIO devices, not automatic routing:

| Component | Role |
|-----------|------|
| `memHierarchy.mmioEx` (`testcpu/standardMMIO.*`) | Example device: `setMemoryMappedAddressRegion(base, size)`; handles Read/Write to that range |
| `memHierarchy.standardInterface` | CPU-side bridge; can cooperate with MMU |
| Coherence / caches | Must be configured so MMIO addresses route to device, not DRAM (address interleaving, ranges on MemController) |

**Typical SST topology for modeled MMIO:**

```
CPU (StandardMem) → Cache → … → Bus
                                  ├→ MemController (DRAM range)
                                  └→ mmioEx (device range)
```

**Tests:** `testsuite_default_memHierarchy_memHA.py` includes `StdMem_mmio`, `StdMem_mmio2`, `StdMem_mmio3`.

### 4.3 MMIO in Quetz

Quetz’s MMIO story is **“filter, don’t model”**:

| `memmap_type` | Behavior |
|---------------|----------|
| `memory` (default) | Forward READ/WRITE to memHierarchy |
| `filtered` | Count `filtered_reads` / `filtered_writes`; **drop** request |
| `uart` | Same as filtered for hierarchy; additionally capture THR write bytes |

**Sysmode pattern** (riscv virt hello / uart echo):

- Mark large low memory `filtered` so firmware’s stack/heap traffic does not pollute cache statistics meant for a specific study region—or conversely, only study DRAM by filtering everything else.
- UART MMIO falls in QEMU’s platform map; plugin sees each LSR poll and THR write as real memory ops.

### 4.3.1 Dedicated MMIO routing (`FORWARD_MMIO`)

P0 adds a second per-vCPU StandardMem port so device doorbells bypass the L1:

| Piece | Role |
|-------|------|
| `MmioForwardRegionHandler` | First-match region; returns `Action::FORWARD_MMIO` |
| `mmio_link_%(vcpu_count)d` | Optional port on `QuetzCPU`; connects to bus or `MemController` |
| `MemRequestEmitter` | `IssuePath::MMIO`: one transaction, no cache-line split |
| Stats | `mmio_read_requests`, `mmio_write_requests`, `mmio_*_latency`, `mmio_truncated_writes` |

**SDL pattern** (see `tests/sysmode/basic_quetz_mmio.py`):

```python
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": 0x80100000, "end": 0x801003FF})
sst.Link("cpu_mmio").connect((cpu, "mmio_link_0", "1ns"), (mmio_dev, "highlink", "1ns"))
```

**Constraints:**

- MMIO write payload is capped at 16 bytes (`QuetzCommand::data`); wider stores increment `mmio_truncated_writes`.
- `cache_link` and `mmio_link` are **not coherent** with each other; packet scratch for GPU/balar (P3) must share a coherent path or use explicit flush/filter rules.
- Place the MMIO handler **before** broad `FilteredRegionHandler` entries so doorbell addresses are not swallowed by a larger filtered range.

### 4.3.2 GPU device endpoint

`quetz.QuetzGpuDevice` is a lightweight accelerator MMIO slave for studies
without GPGPU-Sim. With its `kernel` slot empty, it is a pure-latency model.
With a `QuetzKernel` loaded, it DMA-reads the input, computes, models a BUSY
interval, and DMA-writes the result before reporting completion.
`doorbell_blocking=1` holds a non-posted submit response through writeback;
`doorbell_blocking=0` acknowledges an accepted non-posted submit immediately
while STATUS remains BUSY. Posted writes have no response in either mode.
Kernel mode allows one operation in flight and has no queue; a second submit
while BUSY is fatal. Pure-latency mode retains its bounded queue.

**Register map** (offsets from `base_addr`):

| Offset | Name | Access | Behavior |
|--------|------|--------|----------|
| `0x00` | DOORBELL | Write | Submit an operation |
| `0x08` | STATUS | Read | `1` through kernel DMA-read, compute, and writeback; `0` when idle |
| `0x10` | KERNEL_ID | Read | Count of completed operations |
| `0x18` | LATENCY_OVERRIDE | Write | Per-launch override of modeled latency |
| `0x20` | TICKET | Read | Most recent submit ticket |
| `0x28` | RESULT | Read | Completion latch; mirrors KERNEL_ID in the synthetic model |
| `0x30–0x48` | ARG0–ARG3 | Write | Captured kernel operands |
| `0x50` | IRQ_ACK | Read/Write | Read pending level; write consumes completion events |

**Params and slots:** Alongside `base_addr`, `mmio_size`, `kernel_latency`,
`clock`, and `verbose`, the device exposes `doorbell_blocking`, completion IRQ,
DMA-range, and buffer-byte-order controls. `iface` is the MMIO target;
`mem_iface` is required when the `kernel` slot is populated. The legacy
`dma_bytes_per_kernel` parameter remains unsupported and must be zero.

**Topology:** `iface` connects to the CPU's MMIO path. A real kernel's
`mem_iface` connects to the hierarchy that owns its reviewed DMA buffer window.
That window must be SST-backed; ordinary QEMU RAM is only trace-observed and is
not a coherent second copy for SST device writeback. See
`tests/sysmode/basic_quetz_gpu.py` for the pure-latency model and
`basic_quetz_gpu_compute*.py` for real compute.

**User-mode (same C++ path):** `MmioForwardRegionHandler`, `GpuTraceRegionHandler`, and `QuetzGpuDevice` do not depend on `system_mode`. Wire `tests/usermode/basic_quetz_gpu.py` (device + `mmio_link_0`) or `basic_quetz_gpu_trace.py` (CONSUME only). The guest must `mmap(MAP_FIXED | MAP_ANONYMOUS, …)` at the MMIO base so QEMU user-mode does not fault before the plugin sees loads/stores; host page contents are irrelevant for trace, while the device path needs STATUS reads on `mmio_link`. Test programs: `tests/usermode/sources/gpu_kernel_user.c`, `gpu_trace_user.c`; build with `tests/usermode/sources/build.sh`; testsuite `test_quetz_usermode_gpu_kernel`, `test_quetz_usermode_gpu_trace_capture`. Follow-up: plugin-side address intercept could remove the mmap requirement.

**Stats:** `kernels_launched`, `busy_cycles`, `doorbell_writes`, `status_polls`,
`latency_overrides`, `doorbell_while_busy`, `irqs_raised`, `ops_rejected`,
`wrong_direction_accesses`, and `bad_offset_accesses`.

### 4.4 Implementation problems

| Problem | Description |
|---------|-------------|
| **Filtered ≠ device** | Dropping transactions means **no timing** for MMIO side effects; device state lives only in QEMU |
| **Double simulation** | QEMU fully emulates UART; Quetz optionally observes writes — two truths (QEMU stdout vs `uart_tx_buf_`) |
| **Address space confusion** | User-mode: guest VA; system-mode: often GPA after QEMU softmmu — memmap must use addresses the **plugin reports** |
| **Cache pollution if misconfigured** | Without `filtered`, UART poll loops generate enormous cache traffic (see stale usermode gold vs QEMU ground truth) |
| **No range overlap rules** | `findRegion()` returns first matching region; overlapping memmaps undefined |
| **Reads to devices forwarded by mistake** | If region typed `memory`, LSR polling hits DRAM model — wrong latency and wrong semantics |
| **16-byte store cap** | Device drivers using `str`/`stm` with wide stores may not capture full payload in UART sink |
| **Split across cache lines** | MMIO registers rarely span lines, but generic `issueWrite` could split a device access incorrectly if forwarded |
| **No integration with `mmioEx`** | Use `MmioForwardRegionHandler` + `mmio_link_N` toward `mmioEx` or a bare `MemController` (P0); balar/GPU P3 still needs shared DRAM for packet reads |

**Architectural gap:** Quetz is a **trace feeder**, not an address-space arbiter. Proper MMIO simulation requires either:

- Forwarding MMIO addresses to SST device components, **or**
- Relying entirely on QEMU functional models and excluding those addresses from hierarchy stats (current `filtered` approach).

---

## 5. Driver-Level and Platform Context for Quetz

### 5.1 What “driver level” means here

In Quetz’s target workloads, a **driver** is typically:

- Bare-metal **polling** code in firmware (`riscv_virt_hello.c`)
- Minimal **newlib/libgloss** stubs under QEMU user-mode (syscall translation)
- **OpenSBI + kernel** when running Linux under `qemu-system-riscv64` (not heavily tested in Quetz suite)

Quetz does not intercept syscalls or IRQs unless they manifest as memory ops the plugin sees.

### 5.2 User-mode stack

```
Guest ELF → QEMU user emulation → dynamic linker / musl-glibc
         → syscall → QEMU converts to host syscall
         → guest loads/stores → plugin → Quetz → memHierarchy
```

| Topic | Quetz behavior |
|-------|----------------|
| Syscalls | Handled by QEMU; may touch guest memory (buffers) — seen as loads/stores |
| Threading | `pthread`/OpenMP → multiple vCPUs; each needs `cache_link_N` |
| VDSO | Special mapping; often filtered via memmap in studies |
| ELF loading | QEMU’s job; Quetz only sees post-load execution |

### 5.3 System-mode / bare-metal stack

```
Reset vector → startup.S → C firmware
            → MMIO UART, test finisher
            → while(1) or WFI
```

| Platform | Test binary | Exit mechanism |
|----------|-------------|----------------|
| RISC-V virt | `riscv_virt_hello`, `riscv_virt_uart_echo` | Write to `0x100000` test device; WFI loop |
| ARM M7 MPS2 | `arm_m7_hello` | Semihosting (`-semihosting-config`); QEMU exits sim |
| x86 q35 | `x86_hello` | `isa-debug-exit` port `0x501`; UART via `-serial file:` |

**QEMU machine models** supply:

- Memory map (RAM, ROM, UART, CLINT/PLIC on RISC-V)
- Interrupt controllers (not timing-accurate in Quetz)
- `-bios none` / `-kernel` loading

**Quetz’s role:** Time the **memory hierarchy** for whatever loads/stores the
plugin exports; **not** to replace platform devices. The QEMU overlay also
provides an opt-in functional MCF5208 BSP register-compatibility layer,
separate from SST timing.

### 5.4 Firmware build flow

`tests/sysmode/firmware/build.sh` cross-compiles with
`riscv64-unknown-elf-gcc`, `m68k-linux-gnu-gcc` (ColdFire, the primary
embedded-target scaffold: `coldfire_startup.S`, `link_m68k.ld` with a
`.vectors` section for interrupt-taking firmware, `coldfire_uart.h` /
`coldfire_intc.h`), `arm-none-eabi-gcc`, etc. Linker scripts place sections
at addresses QEMU expects.

### 5.5 Driver-visible gaps

| Gap | Consequence |
|-----|-------------|
| IRQ *timing* | Interrupts work — QEMU-native devices vector normally, and SST-window devices inject INTC lines through the bridge's reverse mailbox — but delivery latency is functional (poll-timer scale), not modeled. Assert function, not timing. |
| No DMA modeling on the trace path | Block copies appear as CPU loads/stores unless the device DMAs inside SST (kernel-compute ops DMA against the SST-backed window) |
| No fence/IO ordering | Weak memory effects not modeled beyond what memHierarchy provides |
| Semihosting | ARM test exits via QEMU magic, not Quetz EXIT command |
| Multi-core firmware | Limited tests; halt quorum handles EXIT per vCPU |
| Board BSP init | Native baseline is RAZ/WI. Discovery plus a reviewed MCF5208 compatibility profile can satisfy basic initialization, but not peripheral timing or data paths — see BSP-COMPATIBILITY.md |

---

## 6. Improving Quetz for Bare-Metal QEMU Simulation

### 6.1 Positioning

Quetz is strongest when the research question is:

> “Given a **fixed binary** and a **memory hierarchy configuration**, what are cache misses, bandwidth, and latency statistics?”

It is weaker when the question requires:

> “Does my **UART driver** meet timing deadlines under interrupt load?” or “Does my **DMA engine** saturate the NoC?”

Improvements should preserve the **lowering barrier** value: run bare-metal ELFs without physical hardware.

### 6.2 High-impact improvements (recommended priority)

*(Status pass 2026-07: much of this list has landed; kept with status
markers as the design rationale.)*

#### P0 — Correctness and trust

1. **Robust memory event capture** — Always register QEMU mem callbacks for decoded ISAs; use classifier only for tagging. Eliminates silent drops (RVC, AMO quirks, future opcodes).
2. **Regenerate / validate gold files** — largely landed: `libmem.so` ground-truth comparison runs in CI (warn-only), gold refresh documented in TESTING.md.
3. **IPC versioning + tests** — partially landed: `tests/unit/test_ipc_layout.cc` pins `QuetzCommand`/`QuetzSharedData`/IRQ-slot layout against the QEMU overlay's C mirror.

#### P1 — Bare-metal workflow

4. **Platform description file (YAML/JSON)** — open. Partial substitute: `platform` presets + deck env conventions.
5. **First-class MMIO integration** — landed and exceeded: `MmioForwardRegionHandler` + `mmio_link_N`, plus the *synchronous* MMIO window (sst-mmio-bridge), SST-side devices (accelerator / stream / sink), and the SST-backed memory window. See README § System mode and § MMIO device components.
6. **Exit detection** — landed: `TestFinisherRegionHandler` sentinel (PASS/FAIL) ends the simulation.

#### P2 — Fidelity knobs

7. **`UartMmioDevice` component** — open as designed; in practice covered otherwise: QEMU-native UARTs are functional (incl. IRQs), TX capture + paced `-serial pipe:` feeds handle observation/stimulus.
8. **IO port plugin support** — open (x86 bare-metal without `-serial` side channel).
9. **Larger store payload** — open; `mmio_truncated_writes` / `cached_truncated_writes` stats make the cap observable.

#### P3 — Usability and scale

10. **Documentation** — landed: SIMULATING-YOUR-SYSTEM.md (tutorial, limits first) + README reference + TESTING.md workflow.
11. **Multi-node** — Merlin test exists for user-mode; extend sysmode multi-core firmware tests.
12. **Checkpoint / replay** — QEMU snapshots + frozen stat windows (hard; long-term).

#### Landed beyond the original list

- **Device IRQ injection** (SST devices raise INTC lines through a reverse
  shared-memory mailbox; m68k vector/INTC firmware scaffold) — was the
  "lock-step for interrupt-driven drivers" long-term item in § 6.5,
  delivered functionally without lock-step.
- **Pluggable compute kernels** (`QuetzKernel` slot: FFT, scale/offset) and
  **accelerator ports** (balar flush-before-doorbell, posted/async engine).
- **Packaging**: `quetz-run` runner + `quetz-sim` runtime image with a CI
  exit-code contract.

### 6.3 Simulation methodology recommendations

| Practice | Reason |
|----------|--------|
| Always define `memmap` for sysmode | Separate DRAM study region from MMIO poll noise |
| Use `exec_latency_*` for sensitivity | Cheap knob without pipeline model |
| Prefer system-mode for bare-metal | User-mode pulls in OS syscall memory traffic |
| Validate event counts vs QEMU `libmem` | Catches classifier regressions early |
| Capture UART via stdout **and** stats | Cross-check functional behavior |
| Do not forward UART MMIO to L1 | Unless modeling a cacheable device region intentionally |

### 6.4 Alternative architectures (when Quetz is not enough)

| Need | Direction |
|------|-----------|
| Full SoC | QEMU **machine** + **hw-riscv-virt** devices tied to SST TLM bridges (major integration) |
| Cycle CPU | Vanadis + memHierarchy; use Quetz only for memory characterization |
| Trace-only | Generate Quetz traces once → replay in SST without QEMU in loop |
| gem5 | Different ecosystem; Quetz fills “QEMU ISA fidelity + SST memory” niche |

### 6.5 Summary vision

**Near-term:** Quetz as a **reliable memory-traffic probe** for QEMU bare-metal and user binaries, with explicit platform maps and no silent plugin drops.

**Medium-term:** Quetz as a **hybrid front-end** — QEMU runs functional devices; SST models **DRAM + selected MMIO devices** with correct address routing.

**Long-term:** Optional **lock-step** or **periodic sync** with QEMU device models for interrupt-driven drivers, without requiring a full in-order CPU pipeline. *(2026-07: the interrupt-driven-driver goal landed without lock-step — synchronous MMIO plus reverse-mailbox IRQ injection; lock-step remains unneeded until someone needs modeled IRQ latency.)*

---

## Appendix A — Quick reference: key parameters

| Parameter | Typical bare-metal use |
|-----------|------------------------|
| `system_mode=1` | `qemu-system-*` + `-machine virt` / `-machine mcf5208evb` |
| `qemu_args` | `-machine virt -nographic -bios none` |
| `system_mode_loader` | `-kernel` for ELF firmware |
| `region_handler` subcomponents | Route/filter/observe address ranges (first match wins) — see README |
| `appstdin` | UART RX test vectors |
| `detailed_instruction_tracking=1` | Per-class compute stats (RISC-V/AArch64) |
| `exec_latency_vec` | Model vector memory pipeline depth |

Launcher env vars (`QUETZ_MMIO_*`, `QUETZ_SST_WIN_*`, `QUETZ_IRQ_*`) are
tabulated in README § Environment variables.

## Appendix B — Source file map

```
quetz/
├── quetzcpu.h / quetzcpu.cc           # SST component: ELI, lifecycle, tick, halt quorum,
│                                      #   sync-MMIO mailbox routing, irq_link handling
├── quetz_config*.{h,cc}               # QuetzConfig(Manager) — parameter parsing + presets
├── quetz_platform_profiles.cc         # Built-in `platform` presets
├── quetz_launcher.h / quetz_launcher.cc  # QemuLauncher — argv (bridge/IRQ devices), fork/exec
├── quetz_qemu_frontend.h / .cc        # SHMParent tunnel owner, child attach/terminate
├── quetzcore.h / quetzcore.cc         # Per-vCPU queue: refillQueue, processQueue
├── quetz_core_backend.h / .cc         # Frontend/backend seam for the core event source
├── quetz_region_handler.h             # MemRegionHandler API
├── quetz_region_handlers.h / .cc      # Forward, Filtered, Uart, MmioForward, GpuTrace, TestFinisher
├── quetz_region_table.h / .cc         # First-match region lookup
├── quetz_pipeline*.{h,cc}             # PipelineInput/Filter/Transform/Output stages + logging
├── quetz_mem_issue.h / quetz_mem_issue.cc  # MemRequestEmitter — line split, StandardMem
├── quetz_mem_access.h / .cc           # Region-table access strategy
├── quetz_stats.h / quetz_stats.cc     # QuetzCoreStats — bundled statistics
├── quetz_shmem.h                      # QuetzTunnel (ring + sync + stats + mmio/irq mailboxes)
├── quetz_ipc_types.h                  # Shared-memory ABI (commands, MMIO slots, IRQ slots)
├── quetz_mmio_sync.h                  # Sync-MMIO response + IRQ-slot publication
├── quetz_irq_event.h                  # QuetzIrqEvent (device irq port -> cpu irq_link)
├── quetz_accelerator_port.h           # AcceleratorHost/AcceleratorPort APIs
├── quetz_balar_accelerator_port.h/.cc # Balar port: doorbell flush, posted/async engine
├── quetz_gpu_device.h / .cc           # Synthetic accelerator (latency model / kernel compute)
├── quetz_kernel_api.h                 # QuetzKernel subcomponent API
├── quetz_fft_kernel.* / quetz_fft.h   # FFT kernel + host-testable math
├── quetz_scale_offset_kernel.* / quetz_scale_offset.h  # Scale/offset kernel + math
├── quetz_stream_device.h / .cc        # Recorded-data feed (pacing, data-ready IRQ)
├── quetz_sink_device.h / .cc          # Write-side capture file
├── qemu_plugin/
│   ├── plugin_main.cpp                # qemu_plugin_install, cb_atexit
│   ├── plugin_state.h / .cpp          # Shared globals, write_cmd
│   ├── instrument.h / .cpp            # cb_tb_trans, mem/exec callbacks
│   ├── insn_classifier.h / .cpp       # Class tagging entry point
│   ├── decoder_riscv.*                # RISC-V + RVC classifiers
│   ├── decoder_aarch64.*              # AArch64 classifier
│   └── decoder_generic.*              # Size-based fallback
├── qemu-overlay/                       # Quetz QEMU sources and pinned patches
│   ├── apply-qemu-overlay.sh           # Apply overlay to upstream QEMU
│   ├── hw/misc/mcf_bsp_compat.c        # Profile-driven Raptor BSP registers
│   ├── hw/misc/raptor_bsp_blocks.h     # Generated block allowlist (board.json)
│   └── hw/misc/sst_mmio_bridge.c       # Synchronous SST MMIO bridge
├── profiles/raptor-init.json           # Example BSP compatibility profile
├── configure.m4 / Makefile.am
├── BSP-COMPATIBILITY.md                # Private-BSP discovery/profile guide
├── GETTING-STARTED.md                 # Docker-only ColdFire V4 on-ramp
├── README.md                          # Reference (components, devices, env vars)
├── SIMULATING-YOUR-SYSTEM.md          # Embedded-system tutorial (limits first)
├── TESTING.md                         # Build + test workflow
├── QUETZ_OUTLINE.md                   # this file
├── tools/                             # Fixture tools + BSP trace/profile analyzer
└── tests/
    ├── unit/                          # doctest units (decoders, IPC layout, kernels, ...)
    ├── usermode/                      # user-mode SDL + golds
    └── sysmode/                       # system-mode SDL, firmware (+ ColdFire scaffold), golds
```

---

*See `README.md`, `GETTING-STARTED.md`, `BSP-COMPATIBILITY.md`, and
in-tree tests for current behavior.*
