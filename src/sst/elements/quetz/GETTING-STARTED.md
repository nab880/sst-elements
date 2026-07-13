# Getting started: testing your ColdFire V4 code in Quetz

This is a fast on-ramp for testing **your own ColdFire V4 firmware** against
Quetz (a QEMU-driven CPU model for SST) — no SST or QEMU experience assumed,
no host install required. Everything runs inside one Docker image.

Every command and transcript below was actually run on this workspace
(macOS/Apple Silicon) while writing this doc — see [Tested on](#tested-on)
at the bottom. Linux should work identically (same Ubuntu-based image, no
CUDA/Rosetta involved), but hasn't been run here yet; if you hit a
difference, it's worth a note back into this file.

If you just want the deep reference docs instead of a walkthrough, jump to
[Where to go next](#where-to-go-next).

## What you need

| Requirement | Notes |
|---|---|
| **Docker** | Desktop (macOS) or Engine (Linux). No SST/QEMU/cross-compiler install needed — they're baked into the image. |
| **Disk** | ~4 GB for the runtime image (`quetz-sim`); ~8 GB if you also build the full test image (`raptor-quetz-test`, see `TESTING.md`) |
| **This workspace** | `raptor-balar/` with its `sst-elements/` and `quetz-docker/` siblings already checked out |

**macOS:** install Docker Desktop, start it, and the commands below just work —
the image builds natively for Apple Silicon (arm64), no Rosetta needed (that's
only required for the separate CUDA/balar image, which you don't need here).

**Linux:** install `docker` (Engine or Desktop) and make sure your user can run
`docker` without `sudo` (or prefix the commands with `sudo`). Same commands,
same image — untested on this workspace so far, but nothing below is
macOS-specific.

All commands assume your shell is at the **workspace root** (the directory
containing `sst-elements/` and `quetz-docker/`).

## Step 1 — build the image (one-time, ~15–20 min)

```sh
docker build --target runtime -t quetz-sim -f quetz-docker/Dockerfile .
```

This builds QEMU 9.2.1 (with the ColdFire/m68k target and Quetz's MMIO
bridge), SST-Core, SST-Elements' `quetz` module, and the `m68k-linux-gnu-gcc`
cross-compiler, all into one image. You only redo this when the QEMU overlay,
Quetz C++, or `Dockerfile` change — not for every firmware edit.

**What the image bakes vs what the mount updates:** `quetz-run` bind-mounts
your checkout's `sst-elements/` tree for **decks, Python helpers, and
firmware** (host mode). **`libquetz.so` and the QEMU plugin live in the
image** at `/opt/sst`. After pulling C++ or overlay changes, rebuild
`quetz-sim` even if your firmware sources are up to date on the host.

## Step 2 — sanity check: run the shipped demo

```sh
./quetz-docker/quetz-run --image quetz-sim --out artifacts/
cat artifacts/transcript.txt artifacts/result.txt
```

Tested output (the `waits=` counters vary with timing — not part of PASS):

```
ColdFire system demo: uart + gps + sensors + accelerator
gps: valid=8 active_fixes=6 waits=345417
sensors: stream=ok rewind=ok waits=0
accel: kernels_completed=2
SYSTEM DEMO PASS
PASS: guest sentinel reported PASS
```

If you see `PASS` here, your Docker setup is good and everything below will
work. `quetz-run` also prints the BSP compatibility scope; see
[Running a private BSP](#step-4--run-a-private-bsp).

## Step 3 — build and run *your* ColdFire V4 code

Two things make code "V4": you compile it with `-mcpu=5475` (the V4e core:
hardware FPU, EMAC, ISA_B) instead of the demo's default `-mcpu=5208` (plain
V2, no FPU), and you run QEMU with `-cpu cfv4e` instead of the default V2
core model. Both sides have to agree, or your code either won't use V4
instructions, or will trap trying to (see
[the #1 mistake](#the-1-mistake-forgetting--cpu-cfv4e) below).

### 3a. Write your firmware

Freestanding C, same shape as any embedded ColdFire program: your entry point
is `kernel_main` (called from the shared reset stub), UART for output, and a
sentinel write to end the simulation PASS/FAIL. Here's a minimal real example
— it actually exercises the V4 FPU (a `double` divide that has no V2
equivalent):

```c
/* my_v4_app.c */
#include <stdint.h>
#include "coldfire_uart.h"

void kernel_main(void)
{
    uart_init();
    uart_puts("hello from my own V4 firmware\n");

    volatile double a = 10.0, b = 4.0;
    double result = a / b;               /* 2.5, needs the V4e FPU */

    uart_puts(result == 2.5 ? "fpu math: ok\n" : "fpu math: BAD\n");

    int pass = (result == 2.5);
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
```

`coldfire_uart.h` (the UART driver + `testdev_done`/`TESTDEV_PASS` sentinel
helper) and the startup scaffold (`coldfire_startup.S`, `link_m68k.ld`) are
shipped in the tree — reuse them rather than writing your own reset code.
Port your real driver/application logic into this shape.

### 3b. Cross-compile it (inside the container — no host toolchain needed)

Put `my_v4_app.c` in its own directory, then, from the workspace root:

```sh
FW=sst-elements/src/sst/elements/quetz/tests/sysmode/firmware
MYCODE=/path/to/your/firmware/dir      # contains my_v4_app.c

docker run --rm \
  -v "$PWD/$FW:/fw:ro" \
  -v "$MYCODE:/work:rw" \
  quetz-sim bash -c '
    cd /work
    cp /fw/coldfire_startup.S /fw/link_m68k.ld /fw/coldfire_uart.h .
    m68k-linux-gnu-gcc -mcpu=5475 -O2 -nostdlib -nostartfiles -ffreestanding \
      -T link_m68k.ld -Wl,--build-id=none \
      coldfire_startup.S my_v4_app.c -o my_v4_app
  '
```

`-mcpu=5475` is a hard-float V4e part, so the compiler emits real ColdFire
FPU/EMAC/ISA_B instructions for code that uses them. (You'll see a linker
note about a missing `.note.GNU-stack` section / executable stack — harmless
and expected for freestanding firmware with no OS loader.)

### 3c. Run it under Quetz with the V4 CPU model

By default `quetz-run` uses the shipped **full system deck**
(`basic_quetz_coldfire_system.py`: GPS replay, sensor stream, accelerator).
That is fine for a UART-only hello-world — unused devices stay idle. For a
minimal UART + sentinel setup, pass an explicit deck (copy
`tests/sysmode/template_system.py` and trim devices, or wire only UART +
TestFinisher like the `coldfire_v4_fpu` test does via env vars).

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware "$MYCODE/my_v4_app" \
  --env "QUETZ_QEMU_ARGS=-machine mcf5208evb -cpu cfv4e -display none -serial stdio -m 128M" \
  --out artifacts/
cat artifacts/transcript.txt artifacts/result.txt
```

When you add SST-side MMIO devices (accelerator, stream, sink) in your own
deck, export `QUETZ_MMIO_PAYLOAD=1` and `QUETZ_MMIO_START`/`QUETZ_MMIO_END`
so the launcher instantiates the bridge — `quetz-run` sets these for the
default demo; custom decks must set them explicitly (see
`SIMULATING-YOUR-SYSTEM.md`).

Tested output:

```
hello from my own V4 firmware
fpu math: ok
PASS: guest sentinel reported PASS
```

That's the full loop: your C, cross-compiled for ColdFire V4, executing real
FPU instructions under QEMU+SST, reporting PASS back out — the same pattern
you'll use for real driver/application code, just swap in your own `.c` file
(and any extra sources on the compile line).

## Step 4 — run a private BSP

You do not need the BSP source. Start with the application ELF, preferably
unstripped and compiled with debug information:

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware /path/to/application.elf \
  --bsp-discover --timeout 30 \
  --out artifacts/bsp-discovery
```

The first run may time out because native QEMU returns zero for unmodeled PLL,
GPIO, watchdog, chip-select, I²C, and related registers. The timeout is still
useful: Quetz writes `bsp-report.txt`, an enriched MMIO trace, and
`bsp-profile.generated.json`.

Review the generated register shapes against the processor manual and board
configuration. The skeleton is intentionally inert—it does not guess status
bits. Then rerun with the reviewed profile:

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware /path/to/application.elf \
  --bsp-profile /path/to/my-board.json \
  --out artifacts/bsp-profiled
```

This can make BSP initialization functionally complete and let you test the
application that follows it. It does not reproduce PLL timing, watchdog
expiry, GPIO stimulus, I²C/QSPI transfers, DMA, or external-bus behavior.
See [BSP-COMPATIBILITY.md](BSP-COMPATIBILITY.md) for the artifact guide,
profile schema, allowed register blocks, and troubleshooting.

## Understanding the output

Every `quetz-run` invocation writes to `--out` (default `./artifacts`):

| File | Contents |
|---|---|
| `transcript.txt` | Your guest's UART/serial output — this is your program talking |
| `result.txt` | `PASS` / `FAIL` / `ERROR` + one-line reason |
| `sst.log` | Full SST stdout/stderr — component logs, the exact QEMU command line, stats |
| `stats.csv` | SST statistics (instruction counts, memory traffic, etc.) |

Exit codes are CI-ready: `0` = guest sentinel reported PASS, `2` = FAIL,
`1` = error/timeout. Your firmware reports PASS/FAIL by calling
`testdev_done(TESTDEV_PASS)` / `testdev_done(TESTDEV_FAIL)` (a store to a
watched sentinel address that ends the simulation).

## The #1 mistake: forgetting `-cpu cfv4e`

If you compile with `-mcpu=5475` but run with the *default* QEMU machine args
(no `-cpu cfv4e`, i.e. you drop `--env QUETZ_QEMU_ARGS=...`), the guest hits
an FPU instruction the default V2 core doesn't have. Tested result: **it
doesn't fail fast** — no transcript, no PASS/FAIL, just a silent hang until
`quetz-run`'s timeout fires (default **600 s**; the repro below uses
`--timeout 30` for a faster failure):

```sh
./quetz-docker/quetz-run --image quetz-sim \
  --firmware "$MYCODE/my_v4_app" \
  --timeout 30 \
  --out artifacts/
# ...
quetz-run: ERROR: timeout after 30s
```

`sst.log` shows QEMU launched fine (`qemu-system-m68k -machine mcf5208evb
... -kernel my_v4_app`, no `-cpu cfv4e`) and then nothing — the trap is
silent from the harness's point of view. If a run hangs with an empty
transcript, check that `-cpu cfv4e` made it into `QUETZ_QEMU_ARGS` before
looking anywhere else.

## What works, what doesn't

**Works today:** freestanding firmware (the shape above), or your
application + drivers ported onto the shipped startup scaffold; QEMU-native
peripherals (UARTs, timers); SST-side MMIO devices (an accelerator, a
sensor-stream device, a data sink) if your workload needs them; both V2
(`-mcpu=5208`) and V4/V4e (`-mcpu=5475` + `-cpu cfv4e`) codegen.

**Private BSPs:** without a profile, unmodeled SoC registers remain
read-as-zero/write-ignored and initialization may hang. Discovery plus a
reviewed compatibility profile can supply the minimum functional register
behavior needed to reach the application. Peripheral data paths still need a
QEMU-native device, an SST MMIO component, or a dedicated new model.

Timing is functional, not cycle-accurate — assert on program behavior
(PASS/FAIL, transcript content), not on instruction counts or cycle timing.

If you later wire an **accelerator with DMA buffers** on a big-endian guest,
read SIMULATING-YOUR-SYSTEM.md § Endianness contract. The ColdFire compute
deck now defaults the SST window and kernel to matching big-endian layout;
custom decks must set both flags together.

## Where to go next

This doc is deliberately narrow (V4, Docker, one file in, one transcript
out). For more:

- **[SIMULATING-YOUR-SYSTEM.md](SIMULATING-YOUR-SYSTEM.md)** — the
  full guide: wiring a UART + sensor-stream + accelerator system, adding your
  own accelerator kernel or MMIO device, interrupt-driven devices, anatomy of
  a deck (SDL). Includes ColdFire V4 assessment notes (`-cpu cfv4e`).
- **[BSP-COMPATIBILITY.md](BSP-COMPATIBILITY.md)** — run a private BSP
  binary, diagnose its initialization accesses, and write a reviewed
  functional register profile.
- **[tests/sysmode/README-coldfire-demo.md](tests/sysmode/README-coldfire-demo.md)**
  — the ColdFire↔GPU (balar) offload demo, and the 32-bit-big-endian vs
  64-bit-little-endian marshalling issues that come up if your V4 code talks
  to an accelerator.
- **[tests/sysmode/template_system.py](tests/sysmode/template_system.py)**
  — copy this as a starting deck if you need a custom device/memory layout
  instead of the default one `quetz-run` uses.
- **`quetz-docker/README.md`** (workspace sibling repo) — the Docker
  build/test harness in full, including the full (non-runtime) test image
  and the regression suite.

## Tested on

- macOS (Apple Silicon / arm64), Docker Desktop, `docker build --target
  runtime` completed in ~17 minutes from a cold cache.
- Verified: shipped demo (Step 2), the V4 FPU/EMAC/ISA_B smoke test
  (`coldfire_v4_fpu`, compiled `-mcpu=5475`, run `-cpu cfv4e`), a from-scratch
  custom firmware compiled and run through the exact Step 3 commands above,
  the "forgot `-cpu cfv4e`" hang, BSP discovery, and a profiled PLL/GPIO
  compatibility run.
- Linux: not yet run against this workspace. The image is a plain
  `ubuntu:24.04` base with no CUDA/GPU dependency for this target, so it
  should build and run the same way — if you try it and something differs,
  update this section.
