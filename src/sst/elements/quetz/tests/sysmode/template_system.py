"""
template_system.py — starting point for simulating YOUR ColdFire system.

Copy this deck, edit the USER CONFIG block, run it with quetz-run:

    quetz-run --deck my_system.py --firmware my_app.elf --out artifacts/

It is the same wiring as the tested reference deck
(basic_quetz_coldfire_system.py — consult it when in doubt): a ColdFire
mcf5208evb guest with a UART console, a data-stream peripheral, and an
accelerator, ending the simulation PASS/FAIL via the TestFinisher sentinel.

Read first: SIMULATING-YOUR-SYSTEM.md — especially what works today vs what
doesn't (freestanding firmware and reviewed BSP profiles yes; peripheral
timing/data paths only when explicitly modeled) and the
supported-parts note. Docs for every register map: the quetz README.
"""

import os
import sst

import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402

# =============================================================================
# USER CONFIG — edit this block.
# =============================================================================

_here = os.path.dirname(os.path.abspath(__file__))

# Your firmware ELF (freestanding; see firmware/coldfire_system.c + build.sh
# for the scaffold). quetz-run --firmware overrides via QUETZ_EXE.
FIRMWARE = os.environ.get("QUETZ_EXE",
                          os.path.join(_here, "firmware", "coldfire_system"))

# Recorded serial input replayed into UART0 RX (GPS NMEA, console script, ...)
# — made with tools/check_nmea.py. Empty = no stdin feed.
SERIAL_INPUT = os.environ.get("QUETZ_STDIN_FILE",
                              os.path.join(_here, "data", "gps_nmea.txt"))

# Recorded byte stream served by the sensor device — made with
# tools/make_stream.py.
SENSOR_STREAM = os.environ.get("QUETZ_SENSOR_FILE",
                               os.path.join(_here, "data", "sensor_stream.bin"))

# Guest memory map. The MMIO window is one sst-mmio-bridge aperture holding
# every SST-side device; give each device a disjoint base inside it. Keep
# QUETZ_MMIO_START/END (quetz-run sets them) matching WINDOW below.
WINDOW_START  = 0x70000000
WINDOW_END    = 0x7001FFFF
ACCEL_BASE    = 0x70000000   # QuetzGpuDevice registers
SENSOR_BASE   = 0x70010000   # QuetzStreamDevice registers
RAM_START     = 0x40000000   # mcf5208evb SDRAM
RAM_END       = 0x47FFFFFF
UART_ADDR     = 0xFC060000   # on-chip UART (QEMU-modeled; trace-observed)
SENTINEL_ADDR = 0x80000000   # TestFinisher: guest writes 0x5555/0x3333 here

# Accelerator compute: None = pure latency model; or a registered
# QuetzKernel, e.g. "quetz.FFTKernel" / "quetz.ScaleOffsetKernel" / yours.
ACCEL_KERNEL = os.environ.get("QUETZ_KERNEL") or None
ACCEL_LATENCY_CYCLES = 5000   # default kernel runtime (latency model)

QEMU_MACHINE_ARGS = "-machine mcf5208evb -display none -serial stdio -m 128M"

# =============================================================================
# Wiring — usually no edits needed below (mirrors the reference deck).
# =============================================================================

sst_home = sst_prefix()
cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu_params = {
    "verbose": 1,
    "clock": os.environ.get("QUETZ_CLOCK", "1GHz"),
    "vcpu_count": 1,
    "maxcorequeue": 64,
    "maxtranscore": 16,
    "maxissuepercycle": 2,
    "cachelinesize": 64,
    "qemu": os.environ.get("QUETZ_QEMU", "qemu-system-m68k"),
    "qemu_plugin": os.environ.get(
        "QUETZ_PLUGIN",
        os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so")),
    "executable": FIRMWARE,
    "system_mode": 1,
    "system_mode_loader": "-kernel",
    "qemu_args": QEMU_MACHINE_ARGS,
}
if SERIAL_INPUT:
    cpu_params["appstdin"] = SERIAL_INPUT
if os.environ.get("QUETZ_STDOUT_FILE"):
    cpu_params["appstdout"] = os.environ["QUETZ_STDOUT_FILE"]
cpu.addParams(cpu_params)

# Region handlers observe the trace stream; first match wins, so the window/
# UART/sentinel entries must precede any broader defaults you add.
rh0 = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
rh0.addParams({"start": WINDOW_START, "end": WINDOW_END})
rh1 = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
rh1.addParams({"start": UART_ADDR, "end": UART_ADDR + 0xFF, "tx_offset": 0x0C})
rh2 = cpu.setSubComponent("region_handler", "quetz.TestFinisherRegionHandler", 2)
rh2.addParams({"start": SENTINEL_ADDR, "end": SENTINEL_ADDR + 3})
# RAM forwards to the MemController explicitly; anything no handler owns —
# a wild access from the buggy firmware you are here to test — is filtered
# into a counted statistic instead of fataling in memHierarchy routing.
rh3 = cpu.setSubComponent("region_handler", "quetz.ForwardRegionHandler", 3)
rh3.addParams({"start": RAM_START, "end": RAM_END})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": RAM_START,
    "addr_range_end": RAM_END,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns",
                  "mem_size": str(RAM_END - RAM_START + 1) + "B"})

accel = sst.Component("accel", "quetz.QuetzGpuDevice")
accel.addParams({
    "base_addr": ACCEL_BASE,
    "mmio_size": 0x400,
    "kernel_latency": ACCEL_LATENCY_CYCLES,
    "clock": "1GHz",
    # Choose blocking submit for this minimal template; a polling/IRQ guest may
    # set 0. Either compute mode also requires mem_iface below.
    "doorbell_blocking": 1 if ACCEL_KERNEL else 0,
})
accel.enableAllStatistics()
accel_if = accel.setSubComponent("iface", "memHierarchy.standardInterface")
if ACCEL_KERNEL:
    accel.setSubComponent("kernel", ACCEL_KERNEL)
    # A kernel DMAs guest buffers itself — wire mem_iface to your memory
    # fabric (see basic_quetz_gpu_compute_coldfire.py for the NoC version).
    raise SystemExit("template: wire accel mem_iface before using a kernel "
                     "(see basic_quetz_gpu_compute_coldfire.py)")

sensors = sst.Component("sensors", "quetz.QuetzStreamDevice")
sensors.addParams({
    "base_addr": SENSOR_BASE,
    "mmio_size": 0x100,
    "stream_file": SENSOR_STREAM,
})
sensors.enableAllStatistics()
sensors_if = sensors.setSubComponent("iface", "memHierarchy.standardInterface")

# One mmio_link, N devices: the bus routes by each device's MMIO range.
mmio_bus = sst.Component("mmio_bus", "memHierarchy.Bus")
mmio_bus.addParams({"bus_frequency": "1GHz"})

sst.Link("cpu_to_mem").connect(
    (cpu, "cache_link_0", "1ns"), (memctrl, "highlink", "1ns"))
sst.Link("cpu_to_mmio_bus").connect(
    (cpu, "mmio_link_0", "1ns"), (mmio_bus, "highlink0", "1ns"))
sst.Link("bus_to_accel").connect(
    (mmio_bus, "lowlink0", "1ns"), (accel_if, "lowlink", "1ns"))
sst.Link("bus_to_sensors").connect(
    (mmio_bus, "lowlink1", "1ns"), (sensors_if, "lowlink", "1ns"))

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
stats_out = os.environ.get("QUETZ_STATS_OUT", "")
if stats_out:
    sst.setStatisticOutput("sst.statOutputCSV", {"filepath": stats_out})
else:
    sst.setStatisticOutput("sst.statOutputConsole")
