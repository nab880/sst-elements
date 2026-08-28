# Simulating your embedded system with quetz

This is the guide for the primary quetz use case: you have code for an
embedded board — a ColdFire-class CPU with a UART console, an accelerator,
a GPS receiver, sensors — and you need to test its **basic functional
correctness without the physical hardware**. The guest executes real
instructions under QEMU and modeled devices return functional data, but this
does not prove behavior for unmodeled hardware or timing.

The worked example throughout is the shipped **`coldfire_system`** demo:

- firmware: `tests/sysmode/firmware/coldfire_system.c`
- deck (SDL): `tests/sysmode/basic_quetz_coldfire_system.py`
- fixtures: `tests/sysmode/data/gps_nmea.txt`, `tests/sysmode/data/sensor_stream.bin`
- test: `test_quetz_coldfire_system` in `tests/testsuite_default_quetz.py`

## What works today — and what doesn't (read this first)

**Works:**

- **Freestanding firmware** (the demo's shape), or your application + drivers
  ported onto the shipped startup scaffold (`coldfire_startup.S` +
  `link_m68k.ld`). This is the supported path.
- **QEMU-native peripherals** — the mcf5208evb's three UARTs, two PIT
  timers, and FEC ethernet — including **interrupt-driven** use: their IRQs
  vector normally through the INTC.
- **Reviewed Raptor functional profile** — `raptor-core2` directly maps the
  confirmed Raptor RAM/ROM apertures, UART0–2, provisional INTC behavior,
  FlexBus/platform registers, GPIOB0–3, and counting DTIM0–3. Unsupported
  MMIO fails closed by default.
- **SST-side MMIO devices** (accelerator, stream, sink, your own), both
  **polled** and **interrupt-driven** — SST devices can raise real INTC
  lines (§ Interrupt-driven devices).
- Recorded-data replay in (stdin/UART, stream device, paced feeds) and
  byte-exact capture out (sink device, UART transcript).

**Works with an explicit compatibility profile:**

- **Private BSP initialization on the legacy vehicle.** Discovery mode logs accesses across
  QEMU's otherwise-unmodeled SCM, bus, chip-select, eDMA, I²C, QSPI, timer,
  EPORT, watchdog, PLL, WTM, and GPIO ranges. The analyzer reports likely
  polling loops and failed write/readback checks from the ELF alone.
- A reviewed JSON profile can add sparse 8/16/32-bit register storage,
  fixed status bits, read sequences, W1C/W1S behavior, and immediate
  same-block write relationships. This is sufficient for many BSPs to finish
  initialization and enter the application.
- The feature is opt-in. With no BSP option, the `bsp_torture` baseline
  remains QEMU's original read-as-zero/write-ignored behavior.
- Compatibility/profile/discovery options are rejected with `raptor-core2`;
  that machine already owns the reviewed devices.

**Still outside the model:**

- PLL frequency and lock timing; watchdog expiry/reset; GPIO stimulus,
  muxing, and pin interrupts; DMA-timer interrupts/DMA requests; I²C/QSPI
  transactions; eDMA movement; external
  FlexBus devices; and hardware-accurate reset sequencing. A profile may
  describe initialization-visible state, not these data paths.
- **Cycle accuracy.** Timing is approximate everywhere (trace-driven memory
  path, functional device latency, IRQ delivery). Never assert timing; assert
  function.

### Running a private BSP

```sh
# Diagnose initialization from the private ELF. A timeout is expected if it polls.
./quetz-docker/quetz-run --firmware my_debug_app.elf \
  --bsp-discover --timeout 30 --out artifacts/discovery

# After reviewing the generated skeleton against the exact hardware:
./quetz-docker/quetz-run --firmware my_debug_app.elf \
  --bsp-profile my-board.json --out artifacts/profiled
```

Discovery produces raw and source-enriched JSONL traces, a diagnosis report,
and a deliberately inert profile skeleton. The complete workflow, artifact
contract, JSON schema, allowed register map, and modeling boundary are in
[BSP-COMPATIBILITY.md](BSP-COMPATIBILITY.md).

## Supported parts

Quetz carries two ColdFire machine roles:

- `mcf5208evb` (MCF5208 V2) is the legacy portability, discovery, and profile
  vehicle.
- `raptor-core2` is the dedicated single-CPU Raptor functional profile. It is
  intentionally a reviewed subset, not an exact SKU claim.

Non-Raptor parts still use relocation on the legacy vehicle:

| your part differs in | you change | cost |
|---|---|---|
| RAM / peripheral bases | `link_m68k.ld` + deck env (`QUETZ_RAM_START/END`, `QUETZ_UART_ADDR`, device `base_addr`s) | trivial |
| UART programming model | usually nothing — the MCF UART block is common across the family | none |
| INTC source numbering | `QUETZ_*_IRQ_LINE` env + `coldfire_intc.h` line constants | trivial |
| core ISA (V2 vs V4/V4e, MAC/EMAC, FPU) | compiler flags + `-cpu cfv4e` in `QUETZ_QEMU_ARGS` — **assessed, CI-gated** (see below) | small |
| on-chip peripherals we don't model | BSP compatibility profile for init; stream/sink/dedicated devices for application behavior | small to large |
| a genuinely different SoC | a new QEMU machine model | expensive — scoped separately |

Demo firmware is built with `-mcpu=5208`; anything ISA_A+-compatible runs
as-is, and V4 builds are covered below.

### ColdFire V4 targets

The target is a **ColdFire V4** core (V4/V4e with EMAC and FPU). For Raptor
BSP ELFs use `raptor-core2`, whose CPU is fixed to `cfv4e`. For generic V4
diagnostics the `mcf5208evb` vehicle remains available with `-cpu cfv4e`.
Verified in the suite:

- **V2-compiled firmware runs unmodified** under `-cpu cfv4e`.
- **V4-native codegen executes correctly**: `-mcpu=5475` (hard-float V4e)
  FPU doubles (mul/add/div, int↔double conversion), ISA_B integer forms,
  and an EMAC multiply-accumulate — `firmware/coldfire_v4_fpu.c`, gated by
  `test_quetz_coldfire_v4_fpu`.
- **Interrupts work on the V4e model**: INTC vectoring, `stop` wake, and
  SST-device IRQ injection — `test_quetz_coldfire_irq_cfv4e`. Isolate the
  INTC register accesses in your driver; silicon offsets may differ from
  the 5208 block.
- **The no-profile BSP baseline is unchanged** under `-cpu cfv4e`
  (`bsp_torture`: still blanket RAZ/WI, no faults). Compatibility profiles
  are currently targeted at the MCF5208 register map even when `cfv4e`
  supplies the core ISA.

On-chip peripherals the legacy vehicle doesn't model read as RAZ/WI without a
compatibility profile. Use profiles only for initialization-visible state;
validate the *data path* with stream/sink devices or a dedicated model.
The memory map is integrator-defined, so relocate via the table above.
QEMU's `cfv4e` does not model the V4 **MMU** (freestanding
/ flat-supervisor firmware — this whole flow — is unaffected; MMU-on OS
validation is out of scope), and it implements ISA_A/B + FPU + EMAC —
`-mcpu=5475` output is the verified configuration.

## Quickstart (three commands)

From a clean checkout of the workspace (with docker):

```sh
docker build --target runtime -t quetz-sim -f quetz-docker/Dockerfile .
./quetz-docker/quetz-run --out artifacts/
cat artifacts/transcript.txt artifacts/result.txt
```

`quetz-run` runs the shipped demo (freestanding firmware — see the BSP
scope note it prints), and leaves `transcript.txt` (guest serial),
`stats.csv`, `sst.log`, and `result.txt` in `--out`. Exit code: 0 = the
guest's sentinel reported PASS, 2 = FAIL, 1 = error/timeout — drop it
straight into CI. Swap in your own pieces with `--firmware my_app.elf`,
`--stdin my_gps.nmea`, `--sensor my_stream.bin`, `--deck my_system.py`
(start from `tests/sysmode/template_system.py`). Fixtures are built from
recordings with `tools/make_stream.py` and `tools/check_nmea.py`.

Running `sst` directly instead (inside the test container):

```sh
export QUETZ_MMIO_PAYLOAD=1 QUETZ_MMIO_START=0x70000000 QUETZ_MMIO_END=0x7001FFFF
sst tests/sysmode/basic_quetz_coldfire_system.py
```

Expected guest transcript (this is the code under test talking; the
`waits=` counters on the GPS/sensor lines vary with timing and are not
part of the PASS contract):

```
ColdFire system demo: uart + gps + sensors + accelerator
gps: valid=8 active_fixes=6
sensors: stream=ok rewind=ok
accel: kernels_completed=2
SYSTEM DEMO PASS
```

## Where peripherals come from

There are two complementary paths. Pick per device:

**1. QEMU-native devices** — whatever your QEMU machine models. For
`-machine mcf5208evb`: the UARTs, timers, and FEC ethernet. These are fully
functional with zero configuration. Recorded input is replayed into UART0 RX
with the `appstdin` component param (the deck's `QUETZ_STDIN_FILE`); TX and RX
are independent directions, so console output and replayed input share UART0
without interfering. This is how the demo's **GPS** works: a recorded NMEA
log goes in via stdin, and the firmware's real NMEA parser (checksum
validation, fix extraction) consumes it exactly as it would consume the
hardware UART.

**2. SST-side MMIO devices** — components mapped into an `sst-mmio-bridge`
window. A guest load/store in the window traps to SST synchronously and the
SST component's answer is what the guest register sees. Use this path for
devices you want to *model, observe, and script*: you get SST statistics,
SDL parameters, and plain C++ behind a tiny component API. The demo uses two:

- `quetz.QuetzGpuDevice` — the **generic accelerator**: a doorbell/status
  latency model (`REG_DOORBELL`, `REG_STATUS`, `REG_KERNEL_ID`,
  `REG_LATENCY_OVERRIDE`, plus async ticket/completion registers). Load a
  compute kernel into its `kernel` subcomponent slot and the device really
  computes: DMA-read from `REG_ARG0`, run the kernel, DMA-write to
  `REG_ARG1` (`quetz.FFTKernel` is the shipped example; write your own by
  subclassing `QuetzKernel` — two methods, see `quetz_kernel_api.h`). Your
  driver code exercises submit → poll → complete against it unchanged.
- `quetz.QuetzStreamDevice` — the **sensor feed**: replays any recorded byte
  stream (samples, telemetry, CAN log) through four registers:

  | offset | reg    | dir | behavior |
  |-------:|--------|-----|----------|
  | 0x00   | STATUS | R   | bytes ready now (unpaced: all remaining) |
  | 0x08   | DATA   | R   | pop up to 4 bytes, packed `b0 | b1<<8 | b2<<16 | b3<<24` |
  | 0x10   | SEQ    | R   | bytes consumed so far |
  | 0x18   | CTRL   | W   | write 1 to rewind (paced: restarts the refill budget) |
  | 0x20   | EOS    | R   | 1 when the stream is fully consumed |

  The packing is numeric, so identical firmware is correct on big-endian
  (ColdFire) and little-endian (RISC-V) guests. Real sensors don't deliver
  everything at t=0: set `pace_bytes` + `pace_period` and STATUS fills over
  sim time, which exercises your polling/timeout logic. The canonical drain
  (correct paced and unpaced — see `coldfire_system.c:sensor_check`):

  ```c
  for (;;) {
      if (STATUS == 0) { if (EOS) break; /* not ready yet */ continue; }
      word = DATA;   /* pops min(4, stream tail) bytes */
  }
  ```

- `quetz.QuetzSinkDevice` — the **actuator/telemetry sink**, the write-side
  mirror of the stream device: whatever your code *emits* (actuator commands,
  processed samples, telemetry frames) is captured byte-exactly to a host
  file (`sink_file` param) for CI assertion — the response half of the
  stimulus → compute → captured-response loop:

  | offset | reg    | dir | behavior |
  |-------:|--------|-----|----------|
  | 0x00   | STATUS | R   | bytes accepted so far (== SEQ, kept for symmetry) |
  | 0x08   | DATA   | W   | push exactly write-size bytes (8/16/32-bit store → 1/2/4), value unpacked low-byte-first |
  | 0x10   | SEQ    | R   | bytes accepted so far |
  | 0x18   | CTRL   | W   | 1 = flush capture to file now; 2 = truncate/restart |

  The file is also written unconditionally at simulation end, so a flush is
  only needed if you want the bytes visible mid-run. `max_bytes` caps the
  capture (drops + counts beyond it) to protect CI disks. Host-side, diff the
  file against a golden or computed expectation (`assert_sink_equals` in
  `quetz_test_helpers.py`); the full-loop demo is
  `firmware/coldfire_accel_sink.c` + `test_quetz_coldfire_accel_sink`:
  recorded stream in → `ScaleOffsetKernel` on the device → sink capture →
  byte-exact host diff.

- **Dedicated serial ports**: the mcf5208evb has three UARTs on `-serial`
  slots. Feed a recording into UART1 at device-realistic line rates with
  `tools/serial_feeder.py` + a `pipe:` chardev (tests: `make_serial_feed`
  helper; deck env: `QUETZ_SERIAL1`). The demo's GPS-on-UART1 variant is
  `firmware/coldfire_system_gps1` (`-DGPS_UART=1`, same source).

Multiple SST devices share **one** bridge window: put a `memHierarchy.Bus`
between the CPU's `mmio_link_0` and the device interfaces and give each device
a disjoint `base_addr` inside the window — the bus routes by address. See the
demo deck's `mmio_bus` wiring.

### Endianness contract (read this if your guest is big-endian)

The bridge mailbox carries **numeric values**, not raw bytes, so a register
access of a given size round-trips exactly on any guest — that is why the
same firmware source is correct on ColdFire (BE) and RISC-V (LE) with no
swapping. Two consequences to know:

- **Device registers** (doorbell, STATUS, DATA, …) are value-semantic. Always
  access a register at one size; mixed-size access to the same register is
  not meaningful (devices count it under `bad_offset`/`wrong_direction`
  statistics).
- **The SST-backed memory window** (`QUETZ_SST_WIN_START/END`) uses the
  component's legacy **little-endian default**. The shipped ColdFire compute
  deck overrides it to big-endian. Same-size access (write u32, read the
  same u32) is exact on a BE guest, but *sub-word aliasing* — writing a word
  and reading its bytes, or `memcpy`ing bytes and reading words — behaves LE,
  the opposite of real big-endian memory. If your firmware does that (real
  driver code staging descriptors or strings byte-wise usually does), keep
  `QUETZ_WIN_BIG_ENDIAN=1` in the ColdFire compute deck or set
  `window_big_endian=1` on the
  CPU **and** `data_big_endian=1` on the `QuetzGpuDevice` (the device pushes
  it into whatever kernel it loads — kernels take no endianness param):
  the window bytes are then stored MSB-first and byte-level layout matches BE
  hardware, while numeric same-size round-trips still hold.

## Anatomy of a deck

Follow `basic_quetz_coldfire_system.py` top to bottom:

1. **CPU**: `quetz.QuetzComponent` with `system_mode=1`, your QEMU binary and
   `qemu_args` (machine, memory, `-serial stdio`), the firmware ELF as
   `executable`, and `appstdin` for replayed serial input.
2. **Region handlers** (trace-side observation; first match wins): an
   `MmioForwardRegionHandler` covering the bridge window, a
   `UartRegionHandler` on the UART for TX capture/stats, and a
   `TestFinisherRegionHandler` on a scratch sentinel address — the guest
   writes `0x5555`/`0x3333` there to end the simulation PASS/FAIL.
3. **Memory**: a `memHierarchy.MemController` covering guest RAM for the
   trace-driven timing path (RAM contents are QEMU's; this only models
   traffic).
4. **Devices + bus** as above.
5. **Environment**: `QUETZ_MMIO_PAYLOAD=1` and `QUETZ_MMIO_START/END` tell the
   launcher to instantiate the bridge aperture over the window.

## Anatomy of the firmware

`coldfire_system.c` is freestanding C built with the ordinary m68k cross
compiler (`firmware/build.sh`, `M68K_CC=m68k-linux-gnu-gcc ./build.sh`):
startup asm + linker script are shared with all ColdFire firmware, UART
helpers come from `coldfire_uart.h`, MMIO is `volatile` pointer access. The
demo's three drivers — NMEA line reader/checksummer, stream drain/verify,
doorbell submit/poll — are exactly the shape of real device drivers, which is
the point: port your driver code into this scaffold, or link your existing
sources against the same startup files.

## Adding your own kernel (model *your* accelerator)

The accelerator's compute is a subcomponent: subclass `QuetzKernel`
(`quetz_kernel_api.h`), implement two methods, register it, and select it in
the SDL — the device's doorbell, DMA, completion-mode, and BUSY-timing
machinery are already done. Shipped examples: `quetz.FFTKernel`
(configured-endian float32, latency `coeff·N·log₂N`) and
`quetz.ScaleOffsetKernel` (saturating int16 sensor transform, latency
`coeff·N`). A minimal custom kernel:

```cpp
#include "quetz_kernel_api.h"

class ChecksumKernel : public SST::Quetz::QuetzKernel {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(ChecksumKernel, "myelem", "ChecksumKernel",
        SST_ELI_ELEMENT_VERSION(1,0,0), "sum32 of a byte buffer",
        SST::Quetz::QuetzKernel)

    ChecksumKernel(ComponentId_t id, Params& p) : QuetzKernel(id, p) {}

    // How many bytes to DMA-read from REG_ARG0. arg2/arg3 are yours.
    uint64_t inputBytes(const SST::Quetz::KernelArgs& a, std::string& err) override {
        if (a.arg2 == 0) { err = "length (REG_ARG2) must be nonzero"; return 0; }
        return a.arg2;
    }

    // Bytes in -> bytes out (DMA-written to REG_ARG1), plus a latency opinion
    // in device cycles (0 = use the device's kernel_latency default; the
    // guest's REG_LATENCY_OVERRIDE beats both).
    uint64_t compute(const SST::Quetz::KernelArgs& a,
                     const std::vector<uint8_t>& in,
                     std::vector<uint8_t>& out) override {
        uint32_t sum = 0;
        for (uint8_t b : in) sum += b;
        out = { (uint8_t)sum, (uint8_t)(sum>>8), (uint8_t)(sum>>16), (uint8_t)(sum>>24) };
        return a.arg2;   // e.g. one cycle per byte
    }
};
```

SDL: `gpu.setSubComponent("kernel", "myelem.ChecksumKernel")` — plus the
device needs `mem_iface` wired. Guest contract: program `REG_ARG0..3`, ring
the doorbell, wait for completion, then read the output buffer. Set
`doorbell_blocking=1` to make a non-posted submit return after writeback, or
leave it at 0 and poll `REG_STATUS`/`REG_KERNEL_ID` (or use the completion
IRQ). Posted writes have no response to hold. Keep your kernel's math in a
standalone header so it can be unit-tested on the host without SST — see
`quetz_fft.h` /
`quetz_scale_offset.h` and their tests under `tests/unit/`.

For oracle-visible lifecycle evidence, set `event_file` to the run's JSONL
artifact path and configure schema-safe `event_source` / `event_operation`
identifiers (for example, `accelerator.fft` / `fft`). The device emits a
requested record at the accepted doorbell and a completed record only after
all writeback responses arrive, or an error record on rejection. It flushes
the terminal record before STATUS becomes idle, KERNEL_ID advances, a held
doorbell releases, or an IRQ is raised. These are lifecycle claims only; the
producer does not fabricate separate DMA-bound events.

## Adding your own device

Copy `quetz_stream_device.{h,cc}` (read-side template) or
`quetz_sink_device.{h,cc}` (write-side template, ~150 lines of logic): a
Component holding a `memHierarchy.standardInterface` subcomponent,
`setMemoryMappedAddressRegion` for its window slice, and a `RequestHandler`
with `handle(Read*)`/`handle(Write*)` implementing the register map. Register
it in `Makefile.am`, give it a disjoint `base_addr`, add a bus `lowlink`,
done. Reads must respond with `read->size` bytes; keep register values ≤32
bits if 32-bit guests will read them.

Fidelity knobs, if you later want *some* timing realism: per-op latency on the
accelerator (`REG_LATENCY_OVERRIDE` / `kernel_latency`), the trace-driven
memory path (per-class latencies on the CPU), and statistics everywhere —
start functional, tighten selectively.

## Interrupt-driven devices (IRQ injection)

Two cases, and the first needs nothing from quetz:

**QEMU-native device IRQs already work.** The machine's own devices vector
normally — mcf5208 UART RX interrupts, PIT timer ticks, FEC — so ISR-driven
code against QEMU peripherals runs unmodified. If your driver takes UART or
timer interrupts, just program the INTC as on hardware (scaffold below).

**SST-window devices can raise guest IRQs too.** The bridge's MMIO path is
synchronous (guest-initiated), so completion used to be poll-only; devices
now drive real interrupt-controller lines through a reverse shared-memory
mailbox the patched QEMU bridge polls (~10 µs of virtual time, functional
latency — never assert timing against it). Devices must hold a line until its
level condition clears; transient pulses shorter than a poll are unsupported.

- `quetz.QuetzGpuDevice`: `irq_line=N` raises line N when an op retires
  (both latency-only and kernel-compute flows, including zero-latency
  completions). Each retire adds one completion *event*; the line stays
  raised while any events remain unconsumed. Ack at `REG_IRQ_ACK` (0x50,
  R: raised / W: consume *N* events, ~0 = all); the line lowers only when
  the event count reaches zero. Retires while the line is already raised
  do not re-send an SST IRQ (the level is already 1) but do increment the
  count — batching several doorbells before servicing the ISR is safe.
- `quetz.QuetzStreamDevice`: `irq_line=N` is high while paced `STATUS > 0`
  and lowers automatically when DATA drains the available count to zero.
  An ACK at `REG_IRQ_ACK` (0x28) cannot lower the line while data remains.
  Requires `pace_bytes > 0`.

Wiring (see `basic_quetz_coldfire_system.py` with `QUETZ_GPU_IRQ_LINE` /
`QUETZ_SENSOR_IRQ_LINE` for a worked example):

1. device param `irq_line` = the machine INTC source number (mcf5208evb free
   sources: 29–35; UARTs own 26–28, PITs 4–5, FEC 36+);
2. an SST Link from the device's `irq` port to a CPU `irq_link_%d` port
   (indices contiguous from 0);
3. `QUETZ_IRQ_LINES=64` in the environment — the launcher passes it to the
   bridge (`irq-count`), which resolves the machine's `mcf-intc` and starts
   the poll timer (other machines: `QUETZ_IRQ_INTC_TYPE` names any
   controller QOM type that exposes qdev GPIO inputs).

Your own device raises lines the same way: send a
`quetz.QuetzIrqEvent{vcpu, line, level}` on an `irq` port (raise on your
event, lower when the guest acks a register you define) — see the ~30 lines
around `raiseIrqOnRetire()` in `quetz_gpu_device.cc`.

**Guest-side scaffold (m68k/ColdFire):** `firmware/coldfire_intc.h` is
everything needed to take interrupts on a freestanding guest — a `.vectors`
table the linker script places at the (1 MB-aligned) VBR base, `cf_vbr_init`,
`cf_irq_install(line, isr)` (vector = 64 + line), `cf_intc_enable(line,
level)` (ICR + CIMR), and the lost-wakeup-free `cf_wait_until(cond)` pattern
built on `stop #0x2000`. Declare ISRs `__attribute__((interrupt_handler))`;
ack the device first, then spin on `cf_intc_pending(line)` until the lower
propagates (one bridge poll tick) so RTE doesn't re-enter on the stale
level — but guard the spin with the device's own line state (read back via
its IRQ_ACK register): if the device still holds or re-raises the line (the
GPU keeps it raised while unconsumed completion events remain; the stream
keeps it raised while STATUS is nonzero),
the IPR bit is legitimately set and the ISR must return so RTE re-takes the
IRQ — an unguarded `while (cf_intc_pending(line));` deadlocks there.
`firmware/coldfire_irq_demo.c` (+ `test_quetz_coldfire_irq`) is the
complete worked example: ISR-driven accelerator completion and data-ready
sensor drain with zero busy-wait polls.
