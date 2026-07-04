# Simulating your embedded system with quetz

This is the guide for the primary quetz use case: you have code for an
embedded board — a ColdFire-class CPU with a UART console, an accelerator,
a GPS receiver, sensors — and you need to **prove it works without the
physical hardware**. Functional fidelity, not cycle accuracy: the guest
executes real instructions under QEMU; devices return real data; timing is
approximate.

The worked example throughout is the shipped **`coldfire_system`** demo:

- firmware: `tests/sysmode/firmware/coldfire_system.c`
- deck (SDL): `tests/sysmode/basic_quetz_coldfire_system.py`
- fixtures: `tests/sysmode/data/gps_nmea.txt`, `tests/sysmode/data/sensor_stream.bin`
- test: `test_quetz_coldfire_system` in `tests/testsuite_default_quetz.py`

## Quickstart (three commands)

From a clean checkout of the workspace (with docker):

```sh
docker build --target runtime -t quetz-sim -f quetz-docker/Dockerfile .
./quetz-docker/quetz-run --out artifacts/
cat artifacts/transcript.txt artifacts/result.txt
```

`quetz-run` runs the shipped demo (freestanding firmware — see the note it
prints about board BSPs), and leaves `transcript.txt` (guest serial),
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

Expected guest transcript (this is the code under test talking):

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
the SDL — the device's doorbell, DMA, blocking, and BUSY-timing machinery are
already done. Shipped examples: `quetz.FFTKernel` (LE float32, latency
`coeff·N·log₂N`) and `quetz.ScaleOffsetKernel` (saturating int16 sensor
transform, latency `coeff·N`). A minimal custom kernel:

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
device needs `mem_iface` wired and `doorbell_blocking=1` (it fatals with
instructions if you forget). Guest contract: program `REG_ARG0..3`, ring the
doorbell (blocking — it returns when the result is in memory), read the
output buffer. Keep your kernel's math in a standalone header so it can be
unit-tested on the host without SST — see `quetz_fft.h` /
`quetz_scale_offset.h` and their tests under `tests/unit/`.

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
latency — never assert timing against it). Level semantics: the device
raises its line on the event of interest and holds it until your ISR writes
the device's `REG_IRQ_ACK`.

- `quetz.QuetzGpuDevice`: `irq_line=N` raises line N when an op retires
  (both latency-only and kernel-compute flows); ack at `REG_IRQ_ACK`
  (0x50, R: raised / W nonzero: ack). Retires while already raised don't
  re-raise — read `REG_KERNEL_ID` in the ISR if you batch.
- `quetz.QuetzStreamDevice`: `irq_line=N` raises when a **paced** refill
  makes STATUS go 0 → nonzero (data ready); ack at `REG_IRQ_ACK` (0x28).
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
level. `firmware/coldfire_irq_demo.c` (+ `test_quetz_coldfire_irq`) is the
complete worked example: ISR-driven accelerator completion and data-ready
sensor drain with zero busy-wait polls.
