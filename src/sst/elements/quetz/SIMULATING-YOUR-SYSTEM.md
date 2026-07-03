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

Run it:

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
  | 0x00   | STATUS | R   | bytes remaining |
  | 0x08   | DATA   | R   | pop up to 4 bytes, packed `b0 | b1<<8 | b2<<16 | b3<<24` |
  | 0x10   | SEQ    | R   | bytes consumed so far |
  | 0x18   | CTRL   | W   | write 1 to rewind |

  The packing is numeric, so identical firmware is correct on big-endian
  (ColdFire) and little-endian (RISC-V) guests.

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

## Adding your own device

Copy `quetz_stream_device.{h,cc}` (~150 lines of logic): a Component holding a
`memHierarchy.standardInterface` subcomponent, `setMemoryMappedAddressRegion`
for its window slice, and a `RequestHandler` with `handle(Read*)`/
`handle(Write*)` implementing the register map. Register it in `Makefile.am`,
give it a disjoint `base_addr`, add a bus `lowlink`, done. Reads must respond
with `read->size` bytes; keep register values ≤32 bits if 32-bit guests will
read them.

Fidelity knobs, if you later want *some* timing realism: per-op latency on the
accelerator (`REG_LATENCY_OVERRIDE` / `kernel_latency`), the trace-driven
memory path (per-class latencies on the CPU), and statistics everywhere —
start functional, tighten selectively.
