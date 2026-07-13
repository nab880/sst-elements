# NXP ColdFire (m68k) serial-console + GPU-offload workflow in Quetz

A real-world embedded workflow on an **NXP ColdFire MCF5208** (big-endian, 32-bit
m68k) CPU, modeled through Quetz (QEMU-TCG + SST plugin). It adds a second host
ISA alongside the existing RISC-V guests — only the CPU/firmware change; the
Quetz component and UART path are arch-agnostic.

Three demonstrations, all driven through `qemu-system-m68k -M mcf5208evb`:

1. **`coldfire_hello`** — bare-metal bring-up: banner over the on-chip UART,
   clean exit.
2. **`coldfire_monitor`** — an interactive **dBUG-style serial console** (the way
   Freescale/NXP ColdFire eval boards are actually used): boot banner, then
   commands read over **UART RX** (`help | peek | poke | dump | run | gpu | quit`)
   with responses over **UART TX**. The `gpu` command dispatches a real GPU job.
3. **`coldfire_gpu`** — non-interactive: boots and offloads a vectorAdd to
   **balar + GPGPU-Sim**, reporting a bit-exact result. A deterministic
   regression vehicle for the wire-ABI marshalling (no UART RX).

## What's hard here: a 32-bit big-endian host vs a 64-bit little-endian wire ABI

balar's `BalarCudaCallPacket_t` ABI was designed for a 64-bit little-endian host
(RISC-V64 matches it exactly). ColdFire is **32-bit big-endian**, so
`coldfire_balar.h` bridges three mismatches entirely guest-side — **no balar
changes**:

1. **Struct layout.** The 536-byte packet uses 8-byte pointers; a 32-bit guest's
   native struct differs. We build the byte layout by hand at the exact 64-bit
   offsets (probed on x86-64), zero-extending guest pointers.
2. **Endianness — the subtle one.** Every multi-byte field is marshalled
   little-endian. *But* the SST plugin captures each store's **value**
   (`qemu_plugin_mem_get_value`) and re-serializes it little-endian into the SST
   memory hierarchy — where balar's DMA reads the packet. So a coalesced 16/32-bit
   store from a big-endian guest is byte-swapped *there* (the guest's own RAM
   round-trips fine via QEMU, which is why `poke`/`peek` work). The fix:
   `st_le32`/`st_le64`/`cb_str` write through `volatile` pointers to force 1-byte
   stores, which are unambiguous.
3. **64-bit MMIO from a 32-bit CPU.** A 32-bit core can't issue an atomic 64-bit
   MMIO transaction, so: the doorbell is a single 32-bit write (balar's
   `dataToUInt64` is size-aware; the scratch address is < 4 GB); the compact
   return is a 32-bit read (GPGPU-Sim device pointers come from
   `GLOBAL_HEAP_START=0x80000000` and fit in 32 bits); the D2H result drains via
   sequential 32-bit reads (balar advances its own offset).

## Arch-agnostic sim termination

`mcf5208evb` has no SiFive test-finisher, so the RISC-V exit (a store to
`0x100000`) doesn't exist (it's ROM on ColdFire). A new
`quetz.TestFinisherRegionHandler` (`MemRegionHandler::Action::END_SIM`) ends the
simulation on a guest store to a configured sentinel — here the on-chip SRAM at
`0x80000000`. The pipeline maps `END_SIM` to `HALT_EXIT`. This is reusable for any
guest machine without a QEMU exit device.

## The doorbell/packet-write ordering barrier (component fix)

Bringing this up surfaced a real, *intermittent* race in the Quetz CPU model
(`Received unknown GPU enum API: 0`, ~2-of-3 runs, load-independent). Two
unsynchronized QEMU→SST channels carry the ordered guest sequence "[write packet
bytes] then [ring doorbell]": packet stores flow through the main tunnel into the
rate-limited pipeline (2 slots/tick), while the doorbell arrives via a separate
synchronous MMIO mailbox polled at the *top* of each SST tick
(`quetzcpu.cc` → `pollMmioSyncMailbox`). The doorbell's cache-line flush could
therefore fire before the packet's *hundreds* of individual byte-stores (the BE
marshalling above) had drained, pushing a partial/zero packet to balar's DMA.
RISC-V hid this — its packet is a handful of wide word-stores, so the window is
tiny — but ColdFire's byte-at-a-time stores blow it wide open.

A firmware barrier *cannot* fix this: the guest's own (QEMU) memory is always
coherent; only SST's shadow memory lags, and the guest has no way to observe or
wait on the SST pipeline. The fix is model-side and arch-agnostic: the balar
doorbell is **armed and deferred** (`quetzcpu_mmio_sync.cc`) — the guest is
already blocked on the synchronous MMIO — until the issuing vCPU `isDrained()`
(`coreQ_` empty and no in-flight writes), at which point `processArmedDoorbells()`
issues the flush and forwards to balar. No deadlock (the pipeline keeps draining
each tick), no balar changes. Validated 30/30 ColdFire + 20/20 RISC-V (FFT still
bit-exact, proving the LE path is undisturbed).

## Files
| File | Role |
|---|---|
| `firmware/link_m68k.ld`, `coldfire_startup.S` | m68k linker script + reset stub (sets %sp, calls C) |
| `firmware/coldfire_uart.h` | ColdFire `mcf_uart` (init/putc/puts/getc/hex/dec) + sentinel |
| `firmware/coldfire_balar.h` | self-contained balar driver: manual 64-bit-LE marshalling + `cb_vadd` |
| `firmware/coldfire_hello.c`, `coldfire_monitor.c`, `coldfire_gpu.c` | the three guests |
| `basic_quetz_balar_coldfire.py` | Quetz→balar fabric SDL (single DRAM range; ColdFire DRAM is below the balar MMIO window) |
| `quetz_region_handlers.{h,cc}` (+ `quetz_pipeline_transform.cc`) | `TestFinisherRegionHandler` / `Action::END_SIM` |

## Build the firmware
```sh
cd src/sst/elements/quetz/tests/sysmode/firmware
M68K_CC=m68k-linux-gnu-gcc ./build.sh   # builds coldfire_{hello,monitor,gpu} (+ the others)
```

## Run the combined monitor + GPU command (the headline)
Run from `balar/tests/` (GPGPU-Sim needs `gpgpusim.config` in the CWD).
```sh
cd src/sst/elements/balar/tests
printf 'help\ngpu\nquit\n' > /tmp/cmds.txt
FW=../../quetz/tests/sysmode/firmware
QUETZ_EXE=$FW/coldfire_monitor \
QUETZ_QEMU=qemu-system-m68k \
QUETZ_QEMU_ARGS='-machine mcf5208evb -display none -serial stdio -m 128M' \
QUETZ_LOADER=-kernel \
QUETZ_STDIN_FILE=/tmp/cmds.txt \
QUETZ_MMIO_PAYLOAD=1 QUETZ_MMIO_START=0x70000000 QUETZ_MMIO_END=0x700005ff \
BALAR_CUDA_EXE_PATH=$PWD/balar_trace/vectorAdd \
  sst ../../quetz/tests/sysmode/basic_quetz_balar_coldfire.py
```
Expected `UART[0]:` transcript:
```
ColdFire dBUG-style monitor (NXP mcf5208evb / m68k)
dbug> help
commands: help | peek <a> | poke <a> <v> | dump <a> [n] | run | gpu | quit
dbug> gpu
dispatching vectorAdd to balar GPU...
gpu vectorAdd correct=256/256
dbug> quit
bye
```
plus `TESTFINISH[0]: ... value=0x5555 (PASS)`.

> **Use `-serial stdio`, not `-nographic`.** `-nographic` muxes the QEMU monitor
> onto the serial (`mon:stdio`), which starves UART RX so the interactive monitor
> never receives typed commands. `-display none -serial stdio` wires UART0
> straight to stdio.

> **`QUETZ_MMIO_PAYLOAD=1` + `QUETZ_MMIO_START/END`** are required: the launcher
> reads these from the *environment* to add `-device sst-mmio-bridge` (the
> synchronous doorbell). Without it the doorbell takes an async path and the
> compact-return read races the packet processing.

For the non-interactive GPU run, point `QUETZ_EXE` at `coldfire_gpu` and drop
`QUETZ_STDIN_FILE`.

## Architectural modeling: same workload, two host ISAs
256-element vectorAdd offloaded to balar/GPGPU-Sim — the host↔GPU **command
protocol is identical** across ISAs; the CPU execution profile differs:

| Quetz stat (vectorAdd) | ColdFire (m68k, 32-bit BE) | RISC-V (rv64, 64-bit LE) |
|---|---|---|
| doorbell MMIO writes | 19 | 19 |
| doorbell flushes | 1216 | 1216 |
| instruction_count | 45,265 | 113,282 |
| read_requests | 1,476 | 14,131 |
| write_requests | 13,492 | 24,222 |

The 19 doorbells / 1216 flushes are the *same* (the same 19 CUDA calls and the
same flush-before-doorbell cost), while the CISC m68k issues far fewer
instructions than RISC-V for the equivalent host-side marshalling — the kind of
host-offload-vs-ISA contrast the Quetz↔balar stack is built to model.

These counts are **invariant under the doorbell-drain fix** above: re-measured
identically post-fix, because deferring the doorbell shifts only *cycle timing*,
not the instruction / memory-request / doorbell event stream. (Numbers from the
non-interactive `coldfire_gpu` vs `riscv_virt_balar_kernel`, both `correct=256/256`.)

## Host↔GPU interconnect model (Quetz-arch P1)

Previously the doorbell and the GPU DMA shared the same generic on-chip router,
so an offload "cost" the same as a cache hop — there was no host↔GPU link model
(`QUETZ-ARCH.md` gap #2). Now balar's doorbell (host MMIO write) and the GPU DMA
engine's host-memory accesses (H2D/D2H) cross a **modeled PCIe/NVLink-style link**
distinct from the NoC, configured in both balar SDLs (on by default):

| env knob | default | meaning |
|---|---|---|
| `QUETZ_GPU_LINK_LATENCY` | `500ns` | one-way latency on the balar-MMIO + DMA-to-host links |
| `QUETZ_GPU_LINK_BW` | `16GB/s` | bandwidth of those GPU-facing links (≈PCIe gen3 x16) |
| `QUETZ_NOC_BW` | `128GB/s` | on-chip NoC bandwidth — raised from the prior 1GB/s placeholder so the PCIe link, not the NoC, binds the GPU path |

The latency is a first-class, monotonic cost (coldfire_gpu, NoC fixed, only the
link latency swept — every point still `correct=256/256`):

| `QUETZ_GPU_LINK_LATENCY` | cpu.cycles |
|---|---|
| `1ns` (≈off) | 428,966 |
| `500ns` (default) | 915,948 |
| `1us` | 1,353,278 |

> **Why the directory MSHR is deepened (16→1024).** balar's `dmaEngine` does not
> handle directory `NACK`s; with the faster NoC the 16-entry directory fills and
> NACKs the DMA, which then aborts (`cannot locate matching request`). Giving the
> directory a deep MSHR avoids the NACK entirely. This is a balar-side robustness
> gap worked around **purely SDL-side — no balar changes**.

## vCPU blocking instead of spinning on offload (Quetz-arch P3)

The synchronous doorbell makes the guest vCPU thread wait for SST to post the
MMIO response. Previously the QEMU bridge **busy-spun** on the shared response
slot (`QUETZ-ARCH.md` gap #3), so a vCPU parked on an offload pinned a host
core for the whole GPU run. Now the bridge blocks in `FUTEX_WAIT` on the slot
and SST `FUTEX_WAKE`s it from `postResponse` — same shared word, no busy-loop
(a 1 ms re-poll makes a missed wake self-heal rather than hang).

Isolating just the wait primitive (40 × 50 ms blocking waits, consumer-thread
CPU vs wall):

| wait_slot | wall | host CPU burned | core busy |
|---|---|---|---|
| busy-spin (old) | 2058 ms | 2014 ms | 97.9% |
| `FUTEX_WAIT` (P3) | 2077 ms | 18 ms | 0.9% |

Same blocking duration, ~110× less host CPU while a vCPU is parked. Correctness
is unchanged (`correct=256/256`); RISC-V is unaffected — a guest still on the
spin build simply ignores the extra wake (no-op on a word no one waits on).

> **Rebuild note.** The wait lives in the QEMU bridge
> (`sst-elements/src/sst/elements/quetz/qemu-overlay/quetz_ipc_client.c`), so the futex path needs
> `qemu-system-*` rebuilt from the overlay; the wake lives in `quetz_mmio_sync.h`
> (rebuilt with the quetz element).

## Compute-run aggregation on the trace ring (Quetz-arch P2)

The plugin used to ship one fixed 96 B ring message **per instruction** —
including a `QUETZ_CMD_NOP` for every non-memory insn (`QUETZ-ARCH.md` gap #2,
throughput). Now the plugin coalesces consecutive same-class compute/branch insns
into one `QUETZ_CMD_COMPUTE_RUN` record (class + count), and the SST input stage
re-expands it into N identical NOP events. The run is flushed before any
memory/MMIO/EXIT command, so program order — and the doorbell drain-gate — are
unchanged; downstream stats are **bit-identical** (`correct=256/256`, FFT
bit-exact, 4/4 testsuite).

Measured ring-message reduction (RISC-V vectorAdd):

| | messages |
|---|---|
| before (1 msg/insn) | 113,282 |
| after (compute runs coalesced) | 88,139 |
| **reduction** | **1.29×** |

The win scales with compute-burst length: vectorAdd is memory-bound (~1.5-insn
runs between loads/stores), so it's modest here; compute-heavy kernels coalesce
far more. The fixed-size SST `TunnelDef` means message-count reduction is the
available lever; shrinking the 96 B payload itself is deferred (needs a
variable-length ring).

## Toolchain (baked into the balar image)
`qemu-system-m68k` (QEMU 9.2.1 built with `m68k-softmmu` + the SST MMIO overlay)
and `gcc-m68k-linux-gnu` (ColdFire via `-mcpu=5208`). See `quetz-docker/Dockerfile.balar`.
