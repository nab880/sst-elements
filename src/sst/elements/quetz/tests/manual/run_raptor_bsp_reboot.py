#!/usr/bin/env python3
"""Diagnostic: invoke the unchanged exported BSP's ROM-vector software reboot.

This calls the original routine, not its self-test main or acceptance oracle.
The fixed routine address is guarded by the complete exported ELF SHA-256.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from run_raptor_boot_reset import BootMachine, BootFailure, expect

ELF_SHA256 = '31d27705cf51d07b1ff5654bb717f5a45c3c5802e86a6048541e0b696533c5f3'
REBOOT = 0x80003f8a


def test_reboot(qemu: str, cc: str, objcopy: str, original: Path):
    if hashlib.sha256(original.read_bytes()).hexdigest() != ELF_SHA256:
        raise BootFailure('BSP ELF hash differs; fixed routine address is invalid')
    with tempfile.TemporaryDirectory(prefix='raptor-bsp-reboot-') as temp:
        base = Path(temp)
        source, linker, elf, raw = (base / name for name in ('reboot.S', 'reboot.ld', 'reboot.elf', 'reboot.rom'))
        source.write_text(r'''
        .text
        .global _start
    _start:
        move.l  %sp,0x07000200
        move.l  0x08000200,%d0
        addq.l  #1,%d0
        move.l  %d0,0x08000200
        cmpi.l  #1,%d0
        bne     returned
        move.l  #0x13579bdf,%d7
        move.l  #0xd18,%d0
        move.l  %d0,0xfc008008
        moveq   #1,%d0
        move.b  %d0,0x8000fc00
        jsr     0x80003f8a
    failed:
        bra     failed
    returned:
        move.l  %d7,0x07000204
        move.l  #0xfc064000,%a0
        move.b  #4,8(%a0)
        lea     banner,%a1
    next:
        move.b  (%a1)+,%d0
        tst.b   %d0
        beq     halted
    ready:
        move.b  4(%a0),%d1
        btst    #2,%d1
        beq     ready
        move.b  %d0,12(%a0)
        bra     next
    halted:
        stop    #0x2700
        bra     halted
    banner:
        .asciz "BOOT\n"
''')
        linker.write_text('ENTRY(_start)\nSECTIONS { .vectors 0 : { LONG(0x4000fc00); LONG(_start); } '
                          '.text 0x100 : { *(.text*) } /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) } }\n')
        subprocess.run([cc, '-mcpu=5483', '-nostdlib', '-nostartfiles', '-Wl,--build-id=none',
                        f'-Wl,-T,{linker}', str(source), '-o', str(elf)], check=True)
        subprocess.run([objcopy, '-O', 'binary', str(elf), str(raw)], check=True)
        machine = BootMachine(qemu, base / 'run', raw, rom=True,
                              device_args=('-device', f'loader,file={original}'))
        try:
            machine.qmp('cont')
            machine.wait_banner(1)
            machine.qmp('stop')
            expect(machine.read('l', 0x08000200), 2, 'BSP reboot jumped back to ROM entry')
            expect(machine.read('l', 0x07000200), 0x4000fc00, 'BSP reboot restored ROM SP')
            expect(machine.read('l', 0x07000204), 0x13579bdf, 'software jump preserved D7')
            expect(machine.read('b', 0x8000fc00), 0, 'BSP reboot cleared shared release flag')
            expect(machine.read('w', 0xfc084018) & 0x80, 0x80, 'BSP reboot GPIOB0 bit7 direction')
            expect(machine.read('w', 0xfc084010) & 0x80, 0, 'BSP reboot GPIOB0 bit7 low')
            expect(machine.read('l', 0xfc008008), 0xd18, 'software reboot preserved peripheral configuration')
            expect(sum(event['event'] == 'RESET' for event in machine.events), 0, 'software reboot is not QMP reset')
        finally:
            machine.close()
        if hashlib.sha256(original.read_bytes()).hexdigest() != ELF_SHA256:
            raise BootFailure('Original BSP ELF changed during diagnostic')
    print('PASS original BSP PLATFORM_reboot: verified ELF, ROM SP/PC handoff, hold flag cleared, CPU/peripherals retained')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='qemu-system-m68k')
    parser.add_argument('--cc', default='m68k-linux-gnu-gcc')
    parser.add_argument('--objcopy', default='m68k-linux-gnu-objcopy')
    parser.add_argument('--bsp-elf', type=Path, required=True)
    args = parser.parse_args()
    test_reboot(args.qemu, args.cc, args.objcopy, args.bsp_elf.resolve())


if __name__ == '__main__':
    main()
