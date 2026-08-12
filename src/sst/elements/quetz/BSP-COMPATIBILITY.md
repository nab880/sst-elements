# ColdFire BSP compatibility

Quetz can run a firmware image that contains a private Raptor (ColdFire V4)
board-support package (BSP) without requiring its source. It discovers which
otherwise unmodeled SoC registers the binary touches, reports likely
initialization hangs, and applies a reviewed JSON description of the minimum
register behavior needed by that BSP.

This is for **functional bring-up testing**: did startup finish, did the
application run, and did its drivers produce the expected results? It does
not model clock timing, watchdog expiry, pin electrical behavior, bus
transactions, or peripheral throughput.

## The three operating modes

| Mode | Command | Unmodeled Raptor registers |
|---|---|---|
| Native baseline | no BSP option | QEMU behavior is unchanged: reads return zero and writes are ignored |
| Discovery | `--bsp-discover` | All supported unmodeled blocks are mapped as logged RAZ/WI regions |
| Compatibility | `--bsp-profile FILE` | Only the blocks and exact register shapes in the reviewed profile gain state or scripted behavior |

The compatibility layer is opt-in. It cannot replace QEMU's native FEC,
INTC, UART, PIT, RCM, or SDRAMC devices, and profiles that try to map outside
the approved unmodeled blocks are rejected.

## Quick workflow

Run these commands from the workspace root.

### 1. Build an image containing the Quetz QEMU overlay

```sh
docker build --target runtime -t quetz-sim -f quetz-docker/Dockerfile .
```

The overlay source is under
`sst-elements/src/sst/elements/quetz/qemu-overlay/`. A change there
requires rebuilding the image because the device is compiled into QEMU.

### 2. Discover what the BSP touches

Use an ELF with symbols and debug information when possible:

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware /path/to/application.elf \
  --bsp-discover \
  --timeout 30 \
  --out artifacts/bsp-discovery
```

A timeout is normal on the first run. A BSP waiting for a PLL lock bit may
never reach the application's PASS/FAIL sentinel, but Quetz still processes
the trace after the timeout.

Discovery happens locally. `quetz-run` stages the ELF beneath the selected
artifact directory and mounts it into the local container; it does not send
the firmware or profile to a service.

### 3. Read the report

| Artifact | Purpose |
|---|---|
| `bsp-mmio.raw.jsonl` | Exact QEMU log: PC, address, block, offset, width, operation, value, and whether the profile knew the register |
| `bsp-mmio.jsonl` | The same log enriched with function and source locations when `addr2line` and debug information are available |
| `bsp-report.txt` | Likely polling loops, write/readback mismatches, and unknown access shapes |
| `bsp-profile.generated.json` | Safe, deliberately inert profile skeleton containing the observed registers |
| `sst.log` | QEMU command line, simulator output, and any profile validation error |

The analyzer calls a repeated read a likely poll after 100 consecutive reads
from the same PC to the same address and width. It calls a read a mismatch
when it follows a same-address, same-width write but returns another value.
These are leads for review, not proof of the BSP's intent.

### 4. Review the generated profile

The generated skeleton sets every register to reset value zero with a
`writable_mask` of zero. It therefore preserves RAZ/WI behavior until a
human compares each observed register with:

- the exact processor reference manual;
- the board schematic and straps;
- the BSP's target configuration, if known;
- the application behavior that needs testing.

Do not copy values from another ColdFire part merely because its register
names look similar.

### 5. Run with the reviewed profile

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware /path/to/application.elf \
  --bsp-profile /path/to/my-board.json \
  --timeout 60 \
  --out artifacts/bsp-profiled
```

Repeat discovery without a profile if execution advances to a new,
previously unseen initialization stage. Grow the profile only as far as
needed to reach and test the application.

## Profile format

A profile is JSON with `version: 1`, target `raptor`, and a list of
sparse blocks:

```json
{
  "version": 1,
  "target": "raptor",
  "blocks": [
    {
      "name": "gpiob3",
      "base": "0xfc090000",
      "size": "0x4000",
      "registers": [
        {
          "name": "INT_STATUS",
          "offset": "0xc",
          "width": 2,
          "reset": "0x1",
          "writable_mask": "0x0"
        }
      ]
    }
  ]
}
```

Numbers may be JSON integers or strings accepted by C's base-aware integer
parser, such as `"0xfc090000"`.

### Register fields

| Field | Required | Meaning |
|---|---|---|
| `name` | no | Diagnostic name |
| `offset` | yes | Byte offset within the block |
| `width` | yes | Exact access width: 1, 2, or 4 bytes |
| `reset` | no | Initial stored value; default zero |
| `writable_mask` | no | Bits copied from an ordinary write; default zero |
| `read_set` | no | Bits forced to one on every read |
| `read_clear` | no | Bits forced to zero on every read |
| `w1c` | no | Bits cleared when written as one |
| `w1s` | no | Bits set when written as one |
| `read_sequence` | no | Values returned on successive reads; the final value remains sticky |
| `on_write` | no | Immediate same-block register updates triggered by a write |

Register identity includes both offset and width. For example, a 16-bit
access does not match a 32-bit register at the same offset. An unmatched
read returns zero and an unmatched write has no effect.

Read processing is:

1. take the stored value, or the next `read_sequence` value;
2. OR in `read_set`;
3. clear `read_clear`;
4. mask to the register width.

Write processing is:

1. update stored bits selected by `writable_mask`;
2. apply `w1c` and `w1s`;
3. evaluate each `on_write` rule against the input value.

An `on_write` rule runs when
`(input & mask) == equals`. Its `target` is another register offset in
the same block; `set` and `clear` modify that target immediately. This is
useful for functional relationships such as “programming PLL control makes
LOCK visible.” It does not introduce elapsed time.

A rule with `mask: 0` and `equals: 0` is unconditional, as in the example.
If omitted, `mask` defaults to all bits and the other trigger fields default
to zero. Keep ordinary writable bits separate from W1C/W1S masks.

## Allowed Raptor blocks

Discovery and profiles are restricted to the sparse peripheral blocks the
Quetz overlay has identified as unmodeled (otherwise-RAZ/WI) IPS space. This
table is **generated** from the board contract `boards/raptor/board.json`
(regions carrying a `bsp_compat` object with model `compat`, the default) into
`qemu-overlay/hw/misc/raptor_bsp_blocks.h`; do not edit it by hand — change the
board contract and regenerate with `tools/gen_bsp_compat_blocks.py`.

| Block | Base | Size |
|---|---:|---:|
| FBCS (FlexBus chip selects) | `0xfc008000` | `0x4000` |
| SCM2 (platform control) | `0xfc040000` | `0x4000` |

The DMA timers **DTIM0–3** (`0xfc070000`–`0xfc07ffff`) and the GPIO banks
**GPIOB0–3** (`0xfc084000`–`0xfc093fff`) are *not* in this allowlist. They are
modeled by dedicated QEMU devices (real behavior, not register storage):
`mcf-dtimer` (a virtual-time counter) and `mcf-gpio` (16-bit registers with
mask-in-high-byte value writes and read-modify-write direction). Their block
tables are generated separately into `qemu-overlay/hw/misc/raptor_dtimer_blocks.h`
and `raptor_gpio_blocks.h` from the same board contract (regions with
`bsp_compat.model` `dtimer` / `gpio`). Both devices are instantiated on the same
activation gate as this compatibility device, so do not add a DTIM or GPIO block
to a compat profile — the two would claim the same aperture.

The block name and base must match this table. A profile may use a smaller
size but cannot extend beyond the canonical range. Profile blocks cannot
overlap. QEMU-native devices (UART0–2, PIT, FEC, INTC) are deliberately absent
— the compat device must never shadow them.

## Starter profile

[`profiles/raptor-init.json`](profiles/raptor-init.json) demonstrates:

- writable FlexBus chip-select configuration registers;
- a stored SCM2 register value for write/readback;
- a fixed SCM2 status bit that satisfies a bounded init poll.

It is an example of the format, not an authoritative Raptor or board model.
Audit every address, width, mask, and status bit before using it with a real
BSP. The reviewed board profile
[`profiles/raptor-board.json`](../../../../../../profiles/raptor-board.json)
(umbrella repo) is the starting point for real Raptor bring-up.

## What this can and cannot validate

It can support:

- startup code that stores configuration and reads it back;
- status polls that need a documented steady value or finite sequence;
- write-one-to-clear or write-one-to-set status handling;
- simple immediate relationships between registers;
- basic application correctness after BSP initialization.

It does not implement:

- PLL frequency changes or lock delay;
- watchdog expiry, reset, or service-window timing;
- GPIO input stimulus, pin muxing, electrical levels, or interrupts (GPIO
  register I/O — direction/value/readback — is modeled by the separate mcf-gpio
  device; interrupts and external pin stimulus are not);
- I²C/QSPI transfers or attached devices;
- eDMA data movement;
- FlexBus external memory/device transactions;
- clock trees, or timer event/interrupt scheduling (DMA-timer *counting* is
  modeled by the separate `mcf-dtimer` device; timer interrupts/DMA are not);
- hardware-accurate reset sequencing or cycle timing.

Use QEMU-native devices where available. Use an SST MMIO component for a
functional peripheral whose data path matters. Add a dedicated QEMU device
when behavior must participate directly in the emulated SoC. A compatibility
profile should remain a small initialization contract, not grow into a
peripheral simulator.

## CLI and launcher interface

`quetz-run` is the supported interface:

| Option | Effect |
|---|---|
| `--bsp-discover` | Enable discovery logging and postprocessing |
| `--bsp-profile FILE` | Stage and apply a version-1 JSON profile |
| `--bsp-target raptor` | Select the target; currently the only accepted value |

For custom launchers, the equivalent system-mode environment variables are:

| Variable | Effect |
|---|---|
| `QUETZ_BSP_DISCOVER=1` | Instantiate the discovery device |
| `QUETZ_BSP_PROFILE=/path/profile.json` | Apply a profile |
| `QUETZ_BSP_TARGET=raptor` | Select the target |
| `QUETZ_BSP_LOG=/path/trace.jsonl` | Write raw access records |

These options are system-mode only. The launcher rejects them in QEMU
user mode.

## Source map

| Path | Responsibility |
|---|---|
| `qemu-overlay/hw/misc/mcf_bsp_compat.c` | QEMU register device, profile parser, access log |
| `qemu-overlay/hw/misc/raptor_bsp_blocks.h` | Generated block allowlist (from `boards/raptor/board.json`) |
| `qemu-overlay/hw/misc/mcf_dtimer.c` | QEMU DMA-timer device (DTIM0–3): virtual-time counter |
| `qemu-overlay/hw/misc/raptor_dtimer_blocks.h` | Generated DTIM block table (model `dtimer` regions) |
| `qemu-overlay/hw/misc/mcf_gpio.c` | QEMU GPIO device (GPIOB0–3): 16-bit, mask-in-high-byte value, RMW direction |
| `qemu-overlay/hw/misc/raptor_gpio_blocks.h` | Generated GPIO block table (model `gpio` regions) |
| `tools/gen_bsp_compat_blocks.py` | Regenerates the allowlist + DTIM/GPIO headers from the board contract |
| `qemu-overlay/apply-qemu-overlay.sh` | Installs the Quetz overlay into pinned QEMU source |
| `quetz_launcher.cc` | Converts environment variables into QEMU device arguments |
| `tools/bsp_profile.py` | Enriches traces, reports likely problems, generates inert skeletons |
| `profiles/raptor-init.json` | Reviewed-example starting profile |
| `tests/sysmode/firmware/bsp_torture.c` | Baseline and profiled behavior probe |

## Troubleshooting

**No BSP artifacts appear:** verify the run is system mode, uses a rebuilt
QEMU overlay, and includes `--bsp-discover`.

**The report has no source lines:** use an unstripped ELF with debug
information and ensure `m68k-linux-gnu-addr2line` or `addr2line` exists.
The PC and address data remain usable without symbols.

**A profiled register still reads zero:** check block base, register offset,
and exact access width in `bsp-mmio.raw.jsonl`.

**QEMU rejects the profile:** read `sst.log`. Invalid JSON, unsupported
targets, duplicate register shapes, overlapping blocks, and ranges outside
the allowlist fail during device realization.

**Initialization advances and hangs elsewhere:** run discovery again without
the profile, inspect the new poll, and extend the reviewed profile.
