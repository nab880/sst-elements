"""
basic_quetz_coldfire_system.py — full embedded-system deck for a ColdFire guest.

The reference SDL for the primary quetz use case: functional validation of
embedded code for a ColdFire (mcf5208evb) board with a UART console, a GPS
receiver, a sensor stream, and a generic accelerator — no physical hardware,
no cycle-accuracy requirement.

Peripheral plumbing:
  UART0 console   QEMU's mcf5208 UART. TX -> stdout (gold-compared); RX is fed
                  the recorded GPS NMEA log via appstdin (TX and RX are
                  independent directions, so console and GPS share UART0).
  Sensors         quetz.QuetzStreamDevice at 0x70010000 replaying
                  data/sensor_stream.bin through STATUS/DATA/SEQ/CTRL registers.
  Accelerator     quetz.QuetzGpuDevice at 0x70000000 (doorbell latency model).
  Both MMIO devices share one sst-mmio-bridge window (0x70000000..0x7001FFFF);
  a memHierarchy.Bus routes by address from the single mmio_link.

Firmware: sysmode/firmware/coldfire_system (build.sh). PASS/FAIL via the
TestFinisher sentinel; the UART transcript is the gold-compared output.

Env overrides: QUETZ_EXE, QUETZ_QEMU(=qemu-system-m68k), QUETZ_QEMU_ARGS,
QUETZ_STDIN_FILE (GPS NMEA log), QUETZ_SENSOR_FILE (sensor stream),
QUETZ_GPU_LATENCY, QUETZ_UART_ADDR, QUETZ_SENTINEL_ADDR.

Optional device IRQ injection (ISR-driven completion instead of polling):
QUETZ_GPU_IRQ_LINE / QUETZ_SENSOR_IRQ_LINE name mcf5208 INTC sources (the
coldfire_irq_demo firmware expects 30 / 31); setting either wires the
device's 'irq' port to a cpu irq_link_%d port and exports QUETZ_IRQ_LINES so
the launcher enables the QEMU bridge's IRQ poll timer.

Optional write-side sink (QUETZ_SINK_FILE): quetz.QuetzSinkDevice at
QUETZ_SINK_BASE (default 0x70020000) captures guest-pushed bytes to that
file — set QUETZ_MMIO_END=0x7002FFFF so the bridge window covers it.
"""

import os
import sst

import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from quetz_test_helpers import sst_prefix  # noqa: E402


def _parse_addr(s):
    s = str(s).strip()
    return int(s, 16) if s.startswith(("0x", "0X")) else int(s)


_here      = os.path.dirname(os.path.abspath(__file__))
sst_home   = sst_prefix()
exe        = os.environ.get("QUETZ_EXE",
                            os.path.join(_here, "firmware", "coldfire_system"))
qemu_bin   = os.environ.get("QUETZ_QEMU", "qemu-system-m68k")
plugin     = os.environ.get("QUETZ_PLUGIN",
                            os.path.join(sst_home, "libexec", "libqemu_sst_plugin.so"))
qemu_args  = os.environ.get("QUETZ_QEMU_ARGS",
                            "-machine mcf5208evb -display none -serial stdio -m 128M")
# Dedicated second serial port (e.g. a paced GPS feed): QUETZ_SERIAL1 carries
# the chardev spec ("pipe:/tmp/.../gps" from make_serial_feed) and lands on
# UART1 (-serial slots map positionally; slot 0 stays the stdio console).
serial1 = os.environ.get("QUETZ_SERIAL1", "")
if serial1:
    qemu_args += " -serial " + serial1
loader     = os.environ.get("QUETZ_LOADER", "-kernel")
clock      = os.environ.get("QUETZ_CLOCK", "1GHz")
stdin_file = os.environ.get("QUETZ_STDIN_FILE",
                            os.path.join(_here, "data", "gps_nmea.txt"))
stdout_file = os.environ.get("QUETZ_STDOUT_FILE", "")
sensor_file = os.environ.get("QUETZ_SENSOR_FILE",
                             os.path.join(_here, "data", "sensor_stream.bin"))

mmio_start = _parse_addr(os.environ.get("QUETZ_MMIO_START", "0x70000000"))
mmio_end   = _parse_addr(os.environ.get("QUETZ_MMIO_END", "0x7001FFFF"))
gpu_base    = _parse_addr(os.environ.get("QUETZ_GPU_BASE", "0x70000000"))
sensor_base = _parse_addr(os.environ.get("QUETZ_SENSOR_BASE", "0x70010000"))
sink_file   = os.environ.get("QUETZ_SINK_FILE", "")
sink_base   = _parse_addr(os.environ.get("QUETZ_SINK_BASE", "0x70020000"))
# Device IRQ injection: INTC source numbers ("" = polled completion, no IRQs).
gpu_irq_line    = os.environ.get("QUETZ_GPU_IRQ_LINE", "")
sensor_irq_line = os.environ.get("QUETZ_SENSOR_IRQ_LINE", "")
if gpu_irq_line or sensor_irq_line:
    # The launcher reads this at QEMU spawn time (same process): enable the
    # bridge's reverse-mailbox poll for all 64 mcf_intc inputs.
    os.environ.setdefault("QUETZ_IRQ_LINES", "64")
ram_start  = _parse_addr(os.environ.get("QUETZ_RAM_START", "0x40000000"))
ram_end    = _parse_addr(os.environ.get("QUETZ_RAM_END", "0x47FFFFFF"))
uart_addr  = _parse_addr(os.environ.get("QUETZ_UART_ADDR", "0xfc060000"))
sentinel_addr = _parse_addr(os.environ.get("QUETZ_SENTINEL_ADDR", "0x80000000"))

if not os.path.exists(exe):
    raise FileNotFoundError(
        "coldfire_system firmware not found at {}; run sysmode/firmware/build.sh".format(exe))
# Without QUETZ_MMIO_PAYLOAD=1 the launcher creates no sst-mmio-bridge, the
# devices below are unreachable (their registers read as zero from QEMU), and
# the firmware spins on SENSOR_STATUS until the timeout — an opaque hang.
if os.environ.get("QUETZ_MMIO_PAYLOAD", "") != "1":
    print("WARNING: basic_quetz_coldfire_system.py: QUETZ_MMIO_PAYLOAD=1 is "
          "not set — the sync-MMIO bridge will NOT be created and the "
          "GPU/sensor devices will be invisible to the guest (reads return "
          "0). If the run hangs polling a device register, this is why.",
          flush=True)
if not os.path.exists(stdin_file):
    raise FileNotFoundError("GPS NMEA fixture not found at {}".format(stdin_file))
if not os.path.exists(sensor_file):
    raise FileNotFoundError("sensor stream fixture not found at {}".format(sensor_file))

cpu_params = {
    "verbose": 1,
    "clock": clock,
    "vcpu_count": 1,
    "maxcorequeue": 64,
    "maxtranscore": 16,
    "maxissuepercycle": 2,
    "cachelinesize": 64,
    "qemu": qemu_bin,
    "qemu_plugin": plugin,
    "executable": exe,
    "system_mode": 1,
    "system_mode_loader": loader,
    "qemu_args": qemu_args,
    "appstdin": stdin_file,
}
if stdout_file:
    cpu_params["appstdout"] = stdout_file

cpu = sst.Component("cpu", "quetz.QuetzComponent")
cpu.addParams(cpu_params)

# First-match-wins; the MMIO window/uart/sentinel must precede the DRAM default.
mmio_rh = cpu.setSubComponent("region_handler", "quetz.MmioForwardRegionHandler", 0)
mmio_rh.addParams({"start": mmio_start, "end": mmio_end})
# Covers all three mcf5208 UARTs (0xfc060000/4000/8000) so a dedicated GPS
# port on UART1 is trace-filtered too; tx_offset still captures UART0 TX only.
uart_rh = cpu.setSubComponent("region_handler", "quetz.UartRegionHandler", 1)
uart_rh.addParams({"start": uart_addr, "end": uart_addr + 0x8FFF, "tx_offset": 0x0C})
fin_rh = cpu.setSubComponent("region_handler", "quetz.TestFinisherRegionHandler", 2)
fin_rh.addParams({"start": sentinel_addr, "end": sentinel_addr + 3})
# The rest of the on-chip peripheral space (INTC, SCM, timers, ...) is
# QEMU-serviced; filter it so e.g. the IRQ demo's INTC programming is not
# forwarded to the RAM-only MemController. Must come after the UART slot.
soc_rh = cpu.setSubComponent("region_handler", "quetz.FilteredRegionHandler", 3)
soc_rh.addParams({"start": 0xFC000000, "end": 0xFCFFFFFF})
# RAM forwards to the MemController explicitly, and EVERYTHING else is
# filtered by the CPU's no-match policy: a wild guest access (the buggy
# firmware a user is here to test) must degrade to a counted statistic, not a
# memHierarchy fatal for an address no controller owns. QEMU itself is RAZ/WI
# for unassigned mcf5208evb space (pinned by test_quetz_coldfire_bsp_torture),
# so the guest survives either way.
ram_rh = cpu.setSubComponent("region_handler", "quetz.ForwardRegionHandler", 4)
ram_rh.addParams({"start": ram_start, "end": ram_end})
cpu.addParams({"filter_unmatched_regions": 1})
cpu.enableAllStatistics()

memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": ram_start,
    "addr_range_end": ram_end,
})
mem_be = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem_be.addParams({"access_time": "100ns", "mem_size": str(ram_end - ram_start + 1) + "B"})

# Accelerator (synthetic doorbell latency model).
gpu_params = {
    "base_addr": gpu_base,
    "mmio_size": 0x400,
    "kernel_latency": int(os.environ.get("QUETZ_GPU_LATENCY", "5000")),
    "clock": "1GHz",
    "doorbell_blocking": 0,
}
if gpu_irq_line:
    gpu_params["irq_line"] = int(gpu_irq_line)
gpu = sst.Component("gpu", "quetz.QuetzGpuDevice")
gpu.addParams(gpu_params)
gpu.enableAllStatistics()
gpu_if = gpu.setSubComponent("iface", "memHierarchy.standardInterface")

# Sensor stream (file-backed data feed). QUETZ_SENSOR_PACE_BYTES paces the
# replay (bytes per QUETZ_SENSOR_PACE_PERIOD); 0 = all available at t=0.
sensor_params = {
    "base_addr": sensor_base,
    "mmio_size": 0x100,
    "stream_file": sensor_file,
    "pace_bytes": int(os.environ.get("QUETZ_SENSOR_PACE_BYTES", "0")),
    "pace_period": os.environ.get("QUETZ_SENSOR_PACE_PERIOD", "100us"),
}
if sensor_irq_line:
    sensor_params["irq_line"] = int(sensor_irq_line)
sensors = sst.Component("sensors", "quetz.QuetzStreamDevice")
sensors.addParams(sensor_params)
sensors.enableAllStatistics()
sensors_if = sensors.setSubComponent("iface", "memHierarchy.standardInterface")

# Write-side sink (optional): captures guest-pushed bytes to QUETZ_SINK_FILE
# for host-side assertion — the response side of the stimulus/response loop.
sink = None
if sink_file:
    sink = sst.Component("sink", "quetz.QuetzSinkDevice")
    sink.addParams({
        "base_addr": sink_base,
        "mmio_size": 0x100,
        "sink_file": sink_file,
    })
    sink.enableAllStatistics()
    sink_if = sink.setSubComponent("iface", "memHierarchy.standardInterface")

# One mmio_link, two devices: a bus routes by each device's MMIO range.
mmio_bus = sst.Component("mmio_bus", "memHierarchy.Bus")
mmio_bus.addParams({"bus_frequency": "1GHz"})

sst.Link("cpu_to_mem").connect(
    (cpu,     "cache_link_0", "1ns"),
    (memctrl, "highlink",     "1ns"))

sst.Link("cpu_to_mmio_bus").connect(
    (cpu,      "mmio_link_0", "1ns"),
    (mmio_bus, "highlink0",   "1ns"))

sst.Link("bus_to_gpu").connect(
    (mmio_bus, "lowlink0", "1ns"),
    (gpu_if,   "lowlink",  "1ns"))

sst.Link("bus_to_sensors").connect(
    (mmio_bus, "lowlink1",  "1ns"),
    (sensors_if, "lowlink", "1ns"))

if sink is not None:
    sst.Link("bus_to_sink").connect(
        (mmio_bus, "lowlink2", "1ns"),
        (sink_if,  "lowlink",  "1ns"))

# Device IRQ links (cpu irq_link indices are contiguous from 0).
_next_irq_link = 0
if gpu_irq_line:
    sst.Link("gpu_irq").connect(
        (gpu, "irq", "1ns"),
        (cpu, "irq_link_%d" % _next_irq_link, "1ns"))
    _next_irq_link += 1
if sensor_irq_line:
    sst.Link("sensors_irq").connect(
        (sensors, "irq", "1ns"),
        (cpu, "irq_link_%d" % _next_irq_link, "1ns"))
    _next_irq_link += 1

sst.setProgramOption("timebase", "1ps")
sst.setStatisticLoadLevel(4)
# QUETZ_STATS_OUT: write statistics as CSV to this path (the quetz-run
# artifacts contract) instead of dumping them on the console.
stats_out = os.environ.get("QUETZ_STATS_OUT", "")
if stats_out:
    sst.setStatisticOutput("sst.statOutputCSV", {"filepath": stats_out})
else:
    sst.setStatisticOutput("sst.statOutputConsole")
