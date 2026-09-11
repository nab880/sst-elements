#!/usr/bin/env python3
"""Exercise explicit ROM-vector boot and repeatable QMP recovery in real QEMU.

Public, generated firmware only. This validates functional boot/reset policy;
it does not validate a private bootloader, silicon watchdog or flash protocol.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


class BootFailure(RuntimeError):
    pass


class BootMachine:
    """Small TCG/QMP/qtest harness; qmp() also supports QOM input controls."""
    def __init__(self, qemu: str, directory: Path, image: Path, *, rom: bool,
                 device_args: tuple[str, ...] = ()):
        directory.mkdir()
        self.directory = directory
        self.uart = directory / 'uart.txt'
        self.log = (directory / 'qemu.log').open('w+')
        self.process = subprocess.Popen([
            qemu, '-machine', 'raptor-core2,strict-mmio=on', '-S',
            '-display', 'none', '-monitor', 'none',
            '-serial', 'null', '-serial', f'file:{self.uart}', '-serial', 'null',
            '-qmp', f'unix:{directory}/qmp.sock,server=on,wait=off',
            '-qtest', f'unix:{directory}/qtest.sock,server=on,wait=off',
            '-qtest-log', '/dev/null', '-bios' if rom else '-kernel', str(image),
            *device_args,
        ], stdout=self.log, stderr=self.log)
        self.sockets: list[socket.socket] = []
        self.streams = []
        self.next_id = 0
        self.events: list[dict] = []
        try:
            self.qmp_stream = self._connect('qmp.sock')
            greeting = json.loads(self.qmp_stream.readline())
            if 'QMP' not in greeting:
                raise BootFailure(f'Unexpected QMP greeting: {greeting}')
            self.qmp('qmp_capabilities')
            self.qtest_stream = self._connect('qtest.sock')
        except Exception:
            self.close()
            raise

    def _connect(self, name: str):
        deadline = time.monotonic() + 5
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sockets.append(endpoint)
        endpoint.settimeout(5)
        while True:
            try:
                endpoint.connect(str(self.directory / name))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.process.poll() is not None or time.monotonic() > deadline:
                    self.log.seek(0)
                    raise BootFailure(f'QEMU did not expose {name}: {self.log.read()}')
                time.sleep(0.01)
        stream = endpoint.makefile('rwb', buffering=0)
        self.streams.append(stream)
        return stream

    def qmp(self, command: str, arguments: dict | None = None):
        self.next_id += 1
        request = {'execute': command, 'id': self.next_id}
        if arguments is not None:
            request['arguments'] = arguments
        self.qmp_stream.write(json.dumps(request).encode() + b'\n')
        while True:
            line = self.qmp_stream.readline()
            if not line:
                raise BootFailure(f'QMP closed during {command}')
            response = json.loads(line)
            if 'event' in response:
                self.events.append(response)
            if response.get('id') == self.next_id:
                if 'error' in response:
                    raise BootFailure(f'{command}: {response["error"]}')
                return response['return']

    def reset(self):
        first = len(self.events)
        self.qmp('system_reset')
        while not any(event['event'] == 'RESET' for event in self.events[first:]):
            raw = self.qmp_stream.readline()
            if not raw:
                raise BootFailure('QMP closed before reset completed')
            response = json.loads(raw)
            if 'event' in response:
                self.events.append(response)

    def qtest(self, command: str) -> str:
        self.qtest_stream.write(command.encode() + b'\n')
        while True:
            response = self.qtest_stream.readline().decode().strip()
            if response.startswith('IRQ '):
                continue
            if not response.startswith('OK'):
                raise BootFailure(f'qtest {command}: {response}')
            return response[2:].strip()

    def read(self, width: str, address: int) -> int:
        return int(self.qtest(f'read{width} 0x{address:x}'), 0)

    def write(self, width: str, address: int, value: int):
        self.qtest(f'write{width} 0x{address:x} 0x{value:x}')

    def wait_banner(self, count: int):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            data = self.uart.read_bytes() if self.uart.exists() else b''
            if data.count(b'BOOT\n') == count:
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.01)
        self.log.seek(0)
        raise BootFailure(f'Expected {count} complete boot banners; {data!r}; {self.log.read()}')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for stream in self.streams:
            stream.close()
        for endpoint in self.sockets:
            endpoint.close()
        self.log.close()


def expect(value, expected, label):
    if value != expected:
        raise BootFailure(f'{label}: expected {expected:#x}, got {value:#x}')


def build_fixture(directory: Path, cc: str, objcopy: str, *, rom: bool,
                  load_base: int = 0x80001000) -> Path:
    prefix = 'rom' if rom else 'elf'
    source = directory / f'{prefix}.S'
    linker = directory / f'{prefix}.ld'
    elf = directory / f'{prefix}.elf'
    source.write_text(r'''
        .text
        .global _start
    _start:
        move.l  %d0,0x07000000
        move.l  %a1,0x07000004
        move.l  %sp,0x07000008
        move.l  #0x8000fc00,%sp
        move.l  0x08000000,%d0
        addq.l  #1,%d0
        move.l  %d0,0x08000000
        move.l  #0x00000d18,%d0
        move.l  %d0,0xfc008008
        moveq   #1,%d0
        move.w  %d0,0xfc088018
        moveq   #3,%d0
        move.w  %d0,0xfc070000
        move.l  #0xfc064000,%a0
        move.b  #4,8(%a0)
        lea     banner,%a1
    next:
        move.b  (%a1)+,%d0
        tst.b   %d0
        beq     finished
    ready:
        move.b  4(%a0),%d1
        btst    #2,%d1
        beq     ready
        move.b  %d0,12(%a0)
        bra     next
    finished:
        move.l  #0x11223344,%d0
        move.l  #0x55667788,%a1
    halted:
        stop    #0x2700
        bra     halted
    banner:
        .asciz "BOOT\n"
''')
    vectors = '. = 0; .vectors : { LONG(0x8000fc00); LONG(_start); }' if rom else ''
    base = '0x100' if rom else f'0x{load_base:x}'
    linker.write_text(f'''ENTRY(_start)
SECTIONS {{
 {vectors}
 . = {base}; .text : {{ *(.text*) }}
 /DISCARD/ : {{ *(.note*) *(.comment*) *(.eh_frame*) }}
}}
''')
    subprocess.run([cc, '-mcpu=5483', '-nostdlib', '-nostartfiles',
                    '-Wl,--build-id=none', f'-Wl,-T,{linker}',
                    str(source), '-o', str(elf)], check=True)
    if not rom:
        return elf
    raw = directory / 'boot.rom'
    subprocess.run([objcopy, '-O', 'binary', str(elf), str(raw)], check=True)
    return raw


def test_reset(qemu: str, base: Path, image: Path, *, rom: bool, gpio_inputs: bool):
    mode = 'ROM' if rom else 'ELF'
    machine = BootMachine(qemu, base / mode.lower(), image, rom=rom)
    try:
        # These locations are outside both generated image segments. Their
        # retention is explicit functional reset policy, not silicon evidence.
        machine.write('l', 0x05000000, 0xA55A1234)
        machine.write('l', 0x07000100, 0xC001CAFE)
        if rom:
            expect(machine.read('l', 0), 0x8000FC00, 'ROM initial SP vector')
            machine.write('l', 0, 0xDEADBEEF)
            expect(machine.read('l', 0), 0x8000FC00, 'ROM rejects guest writes')
        if gpio_inputs:
            machine.qmp('qom-set', {'path': '/machine/gpio', 'property': 'input-mask', 'value': 0x00010000})
            machine.qmp('qom-set', {'path': '/machine/gpio', 'property': 'input-levels', 'value': 0x00010000})
        for boot in (1, 2, 3):
            machine.qmp('cont')
            machine.wait_banner(boot)
            machine.qmp('stop')
            expect(machine.read('l', 0x07000000), 0, f'{mode} reset clears D0')
            expect(machine.read('l', 0x07000004), 0, f'{mode} reset clears A1')
            expect(machine.read('l', 0x07000008), 0x8000FC00 if rom else 0,
                   f'{mode} initial SP policy')
            expect(machine.read('l', 0x08000000), boot, f'{mode} boot counter retention')
            expect(machine.read('l', 0x05000000), 0xA55A1234, 'mpflash retention')
            expect(machine.read('l', 0x07000100), 0xC001CAFE, 'unloaded SRAM retention')
            expect(machine.read('l', 0xFC008008), 0xD18, 'firmware set FlexBus')
            expect(machine.read('w', 0xFC088018), 1, 'firmware set GPIO direction')
            if boot == 3:
                break
            machine.reset()
            expect(machine.read('l', 0xFC008008), 0, 'reset clears FlexBus')
            expect(machine.read('w', 0xFC088018), 0, 'reset clears GPIO direction')
            expect(machine.read('w', 0xFC070000), 0, 'reset disables DTIMER')
            expect(machine.read('l', 0xFC07000C), 0, 'reset clears DTIMER count')
            expect(machine.read('b', 0xFC064004), 8, 'reset disables UART transmitter')
            expect(machine.read('l', 0xFC04800C), 0xFFFFFFFF, 'reset masks interrupts')
            if gpio_inputs:
                # Reset directions are outputs. Host stimulus remains set,
                # then becomes visible when software selects an input again.
                expect(machine.qmp('qom-get', {'path': '/machine/gpio', 'property': 'input-levels'}),
                       0x00010000, 'external input configuration survives reset')
                machine.write('w', 0xFC08C018, 1)
                expect(machine.read('w', 0xFC08C010) & 1, 1, 'external input survives reset')
        print(f'PASS {mode}: three boots, CPU/peripheral reset, ROM protection and memory retention')
    finally:
        machine.close()


def test_invalid_images(qemu: str, base: Path, elf: Path):
    for label, data, diagnostic in (
        ('truncated', b'\0' * 8, 'SP/PC vectors and code'),
        ('odd-pc', struct.pack('>II', 0x8000FC00, 9) + b'\x4e\x71', 'reset PC'),
        ('outside-pc', struct.pack('>II', 0x8000FC00, 0x100) + b'\x4e\x71', 'reset PC'),
        ('bad-sp', struct.pack('>II', 0xFC064000, 8) + b'\x4e\x71', 'reset SP'),
        ('odd-sp', struct.pack('>II', 0x8000FC01, 8) + b'\x4e\x71', 'reset SP'),
        ('ambiguous', struct.pack('>II', 0x8000FC00, 8) + b'\x4e\x71', 'not both'),
    ):
        image = base / f'{label}.rom'
        image.write_bytes(data)
        command = [qemu, '-machine', 'raptor-core2', '-display', 'none',
                   '-serial', 'null', '-bios', str(image)]
        if label == 'ambiguous':
            command += ['-kernel', str(elf)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        if result.returncode == 0 or diagnostic not in result.stderr:
            raise BootFailure(f'{label} unexpectedly accepted: {result.stderr}')
    print('PASS invalid boot images: truncated, unaligned, unmapped and ambiguous inputs rejected')


def test_primary_image_isolation(qemu: str, base: Path, cc: str, objcopy: str):
    outside = base / 'outside-primary'
    outside.mkdir()
    image = build_fixture(outside, cc, objcopy, rom=False, load_base=0x40001000)
    result = subprocess.run(
        [qemu, '-machine', f'raptor-core2,secondary-kernel={image}', '-smp', '2', '-display', 'none',
         '-serial', 'null', '-kernel', str(image)],
        capture_output=True, text=True, timeout=5)
    if result.returncode == 0 or 'two-core primary ELF segments and entry' not in result.stderr:
        raise BootFailure(f'Primary image could overlap secondary RAM: {result.stderr}')
    print('PASS dual-core primary image isolation: P2 load rejected before secondary loading')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='qemu-system-m68k')
    parser.add_argument('--cc', default='m68k-linux-gnu-gcc')
    parser.add_argument('--objcopy', default='m68k-linux-gnu-objcopy')
    parser.add_argument('--gpio-inputs', action='store_true', help='also assert configured external GPIO level survives reset')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='raptor-boot-') as directory:
        base = Path(directory)
        rom = build_fixture(base, args.cc, args.objcopy, rom=True)
        elf = build_fixture(base, args.cc, args.objcopy, rom=False)
        test_reset(args.qemu, base, rom, rom=True, gpio_inputs=args.gpio_inputs)
        test_reset(args.qemu, base, elf, rom=False, gpio_inputs=args.gpio_inputs)
        test_invalid_images(args.qemu, base, elf)
        test_primary_image_isolation(args.qemu, base, args.cc, args.objcopy)


if __name__ == '__main__':
    main()
