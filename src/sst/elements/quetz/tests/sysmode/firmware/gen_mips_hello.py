#!/usr/bin/env python3
"""
gen_mips_hello.py — generate a raw MIPS32 big-endian binary for the
QEMU Malta machine (4Kc CPU).

Loaded as: qemu-system-mips -machine malta -cpu 4Kc -bios mips_malta_hello.bin

Memory layout (Malta, KSEG1 uncached):
  FPGA base at physical 0x1F000000 = KSEG1 0xBF000000
  FPGA CBUS UART NS16550 at FPGA+0x900 = KSEG1 0xBF000900 (regshift=3)
    THR at 0xBF000900, LSR at 0xBF000928
  FPGA SOFTRES at FPGA+0x500 = KSEG1 0xBF000500
    write 0x42 → reset, write 0x4d → clean shutdown (SST custom patch)
  ROM (BIOS) loaded at physical 0x1FC00000 = KSEG1 0xBFC00000

The CPU starts at 0xBFC00000 (physical 0x1FC00000).
The CBUS UART is directly memory-mapped by QEMU; no GT64120/PCI setup needed.
QEMU's Malta UART is always-ready so we skip the LSR polling.
"""

import struct
import sys
import os

# Big-endian MIPS32 instruction encoding helpers
def pack(instr): return struct.pack('>I', instr & 0xFFFFFFFF)
def lui(rt, imm):      return pack(0x3C000000 | (rt  << 16) | (imm & 0xFFFF))
def ori(rt, rs, imm):  return pack(0x34000000 | (rs  << 21) | (rt  << 16) | (imm & 0xFFFF))
def addiu(rt,rs,imm):  return pack(0x24000000 | (rs  << 21) | (rt  << 16) | (imm & 0xFFFF))
def sb(rt, off, rs):   return pack(0xA0000000 | (rs  << 21) | (rt  << 16) | (off & 0xFFFF))
def sw(rt, off, rs):   return pack(0xAC000000 | (rs  << 21) | (rt  << 16) | (off & 0xFFFF))
def j(target):
    idx = (target >> 2) & 0x03FFFFFF
    return pack(0x08000000 | idx)
def nop(): return pack(0x00000000)

# Registers
ZERO, T0, T1, T2, T3 = 0, 8, 9, 10, 11

# LOAD_ADDR: physical 0x1FC00000 mapped at KSEG1 0xBFC00000
LOAD_ADDR  = 0xBFC00000
# FPGA CBUS UART: physical 0x1F000900, KSEG1 0xBF000900 (regshift=3, THR at offset 0)
# This UART is directly memory-mapped by QEMU's Malta init — no GT64120/PCI needed.
UART_BASE  = 0xBF000900
SOFTRES    = 0xBF000500   # KSEG1: Malta FPGA base 0xBF000000 + offset 0x500

MESSAGE = b"Hello from MIPS Malta 4Kc!\n"

def build():
    code = b""

    # --- Set up UART base in $t0 ---
    # UART_BASE = 0xBF000900: FPGA CBUS UART, physical 0x1F000900
    # lui/ori builds 0xBF000900 in two steps.
    code += lui(T0, 0xBF00)           # $t0 = 0xBF000000
    code += ori(T0, T0, 0x0900)       # $t0 = 0xBF000900

    # --- Write each character (CBUS UART THR at offset 0, regshift=3) ---
    for ch in MESSAGE:
        code += addiu(T3, ZERO, ch)   # $t3 = ASCII code
        code += sb(T3, 0, T0)         # MEM[$t0+0] = $t3  (UART THR)

    # --- Exit via Malta FPGA SOFTRES at offset 0x500 (0x4d = clean shutdown) ---
    # Writing 0x42 to SOFTRES triggers a system reset (reboots QEMU).
    # Writing 0x4d triggers a clean shutdown and causes QEMU to exit.
    # The 0x4d value is a custom extension in the SST QEMU patch.
    code += lui(T1, 0xBF00)           # $t1 = 0xBF000000
    code += ori(T1, T1, 0x0500)       # $t1 = 0xBF000500  (SOFTRES = FPGA+0x500)
    code += addiu(T3, ZERO, 0x4d)     # $t3 = 0x4d (SOFTRES shutdown)
    code += sw(T3, 0, T1)             # MEM[$t1] = 0x4d → shutdown → exit

    # --- Spin loop (fallback if reset doesn't fire immediately) ---
    spin = LOAD_ADDR + len(code)
    code += j(spin)
    code += nop()                     # branch delay slot

    # Pad to 4 KiB (enough for the BIOS ROM region)
    code = code.ljust(4096, b'\x00')
    return code


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "mips_malta_hello.bin")
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"Generated {out} ({len(data)} bytes, message: {MESSAGE!r})")
