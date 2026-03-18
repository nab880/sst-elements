# Creating a Custom InterceptionAgent for Carcosa + Vanadis

This guide walks you through converting a plain C program into one that runs
under SST/Vanadis with **Hali MMIO interception**. The key idea: Hali sits in
the CPU's data path and intercepts reads/writes to a small MMIO region. An
**InterceptionAgent** (a C++ SST subcomponent) decides *what* the CPU should
execute next, while the CPU-side C code becomes a thin **finite-state-machine
(FSM) runner** that asks the agent for the next command, executes it, and
reports back.

### Key Points

- **`handleInterceptedEvent()`** is the heart of your agent. It is called for
  every MMIO access. You must respond to reads (`GetS` at `+0x0`) with a
  command value and acknowledge writes (`Write`/`GetX` at `+0x4`).
- **Always `delete` the incoming `MemEvent`** after you create and send a
  response. Failing to do this will leak memory.
- **The MMIO base address** (`0xBEEF0000` by default) must match in three
  places: `hyades.h` (or `#define HYADES_MMIO_BASE`), the Hali
  `intercept_ranges` param, and your agent's `controlAddrBase_`.
- **`agentSetup()`** is called during SST's setup phase — initialize your FSM
  state here.
- **Extra MMIO registers** are easy to add. For example, VLAAgent uses offset
  `+0x8` as a sequence-length register that the C code reads to get dynamic
  state from the agent. Just add another `if (offset == ...)` branch in
  `handleInterceptedEvent()` and a matching `volatile` read in the C code:
  ```c
  #define HYADES_SEQ_LEN_OFFSET 8
  int seq_len = *(volatile int*)(HYADES_MMIO_BASE + HYADES_SEQ_LEN_OFFSET);
  ```
---

## Architecture Overview

```
 ┌──────────────────────────────────────────────────────────┐
 │  CPU-side C binary (runs on Vanadis)                     │
 │                                                          │
 │  hyades_run(jump_table, N)                               │
 │    loop:                                                 │
 │      cmd = *(volatile int*)(MMIO_BASE + 0x0)   ← read   │
 │      if cmd < 0 → exit                                   │
 │      jump_table[cmd]()                                   │
 │      *(volatile int*)(MMIO_BASE + 0x4) = cmd   ← write  │
 └──────────────────┬───────────────────────────────────────┘
                    │ memory accesses
                    ▼
 ┌──────────────────────────────────────────────────────────┐
 │  Hali Component (SST C++)                                │
 │  • Sits between CPU and cache                            │
 │  • Normal memory passes through transparently            │
 │  • Addresses in intercept_ranges → handed to agent       │
 └──────────────────┬───────────────────────────────────────┘
                    │
                    ▼
 ┌──────────────────────────────────────────────────────────┐
 │  InterceptionAgent (your SST subcomponent)               │
 │  • handleInterceptedEvent(): respond to MMIO reads/writes│
 │  • Contains the FSM logic that decides which command to  │
 │    send next and when to exit                            │
 └──────────────────────────────────────────────────────────┘
```

**MMIO protocol (must match between agent and C binary):**


All of this is provided by `hyades.h`, which your C code `#include`s.

---

## Walkthrough: PingPong Example

### Step 1 — Start with a Plain C Program

Suppose you have a simple C program that alternates between two actions:

```c
/* simple_pingpong.c — runs on a normal system, no SST */
#include <stdio.h>

static void ping(void) { printf("PING\n"); }
static void pong(void) { printf("PONG\n"); }

int main(void) {
    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0)
            ping();
        else
            pong();
    }
    return 0;
}
```

This works natively, but there is no external coordination — the control flow
is entirely inside the C binary. To run this under Vanadis with Hali
intercepting and controlling the execution order, you need to:

1. **Convert the control flow into a finite-state-machine (FSM)** where each
   action is a numbered state.
2. **Create an InterceptionAgent** (C++ SST subcomponent) that implements the
   FSM logic and tells the CPU which action to run next.
3. **Replace the loop** in your C code with `hyades_run()`, which reads
   commands from MMIO, dispatches to the right function, and writes status back.

---

### Step 2 — Identify Your States (FSM Design)

Look at your original code and extract each distinct action into its own
function. Assign each one a numeric index:

| Index | Action | Function |
|-------|--------|----------|
| 0     | Ping   | `ping()` |
| 1     | Pong   | `pong()` |

The FSM logic is: alternate 0 → 1 → 0 → 1 … for `max_iterations` rounds, then
send `-1` (exit).

---

### Step 3 — Convert the C Code to Use Hyades

Replace the hard-coded loop with a **jump table** and `hyades_run()`:

```c
/* pingpong.c — Vanadis + Hali version */
#include "hyades.h"
#include <unistd.h>

static void ping(void) { write(1, "PING\n", 5); }
static void pong(void) { write(1, "PONG\n", 5); }

int main(int argc, char *argv[]) {
    int role = hyades_role_from_argv(argc, argv);

    hyades_handler_t jump_table[2];
    if (role == 0) {
        jump_table[0] = ping;
        jump_table[1] = pong;
    } else {
        jump_table[0] = pong;
        jump_table[1] = ping;
    }

    hyades_run(jump_table, 2);
    return 0;
}
```

**What changed:**
- The `for` loop is gone — `hyades_run()` is the loop now.
- Each action is a `void (*)(void)` function in a **jump table** indexed by
  command number.
- `hyades_run()` repeatedly reads the command register (MMIO `+0x0`), calls
  `jump_table[cmd]()`, then writes the status register (MMIO `+0x4`).
- The CPU no longer decides *what* to run — the **agent** does.

**Build** (cross-compile for RISC-V, from the `tests/` directory):

```bash
riscv64-unknown-linux-gnu-gcc -static -I.. -o pingpong pingpong.c
```

The `-I..` flag is needed so the compiler can find `hyades.h` in the parent
`carcosa/` directory.

---

### Step 4 — Create the InterceptionAgent (C++ SST Subcomponent)

Now create the agent that drives the FSM from the simulator side.

#### 4a. Header file: `Components/PingPongAgent.h`



#### 4b. Implementation file: `Components/PingPongAgent.cc`

The core logic lives in `handleInterceptedEvent()`. Hali calls this method
whenever the CPU accesses an address in the intercepted range.

```cpp

bool PingPongAgent::handleInterceptedEvent(MemEvent* ev, Link* highlink)
{
    uint64_t offset = ev->getAddr() - controlAddrBase_;

    // --- Command register read (offset +0x0, GetS) ---
    if (offset == 0x0000 && ev->getCmd() == Command::GetS) {
        if (nextCommand_ >= -1 && nextCommand_ != INT_MIN) {
            // We have a command ready — respond immediately
            sendCommandResponse(ev, nextCommand_);
            nextCommand_ = INT_MIN;
        } else {
            // No command yet — park the request until checkBothDone() fires
            pendingCommandRead_ = ev;
        }
        return true;
    }

    // --- Status register write (offset +0x4, Write/GetX) ---
    if (offset == 0x0004 &&
        (ev->getCmd() == Command::Write || ev->getCmd() == Command::GetX)) {
        sendWriteAck(ev);
        localDone_ = true;
        currentIteration_++;
        // Notify partner via the Hali ring
        if (leftHaliLink_)
            leftHaliLink_->send(
                new HaliEvent("done", (unsigned)currentIteration_));
        checkBothDone();
        return true;
    }

    delete ev;
    return true;
}

void PingPongAgent::checkBothDone()
{
    if (!localDone_ || !partnerDone_) return;

    localDone_   = false;
    partnerDone_ = false;

    // Decide next command
    if (currentIteration_ >= maxIterations_)
        nextCommand_ = -1;          // signal exit
    else
        nextCommand_ = currentIteration_ % 2;  // alternate 0, 1, 0, 1 …

    // If the CPU is already waiting, respond now
    if (pendingCommandRead_) {
        sendCommandResponse(pendingCommandRead_, nextCommand_);
        pendingCommandRead_ = nullptr;
        nextCommand_ = INT_MIN;
    }
}


```

---

### Step 5 — Register Your Agent in the Build System

Add your new `.cc` and `.h` files to `Makefile.am`:

**In `libcarcosa_la_SOURCES`** (the compiled sources list):


**In `nobase_sst_HEADERS`** (installed headers, so other elements can use it):




---

### Step 6 — Write the SST Python Configuration

The Python test script wires everything together. The key section is where
Hali is created and the agent is loaded as a subcomponent:

```python
# Inside your CPU_Builder.build() method:

# Create Hali in the data path between CPU and TLB
hali = sst.Component(prefix + ".hali", "Carcosa.Hali")
hali.addParams({
    "intercept_ranges": "0xBEEF0000,4096",   # must match HYADES_MMIO_BASE
    "verbose": "true",
})

# Load YOUR agent as a subcomponent of Hali
agent = hali.setSubComponent("interceptionAgent", "Carcosa.PingPongAgent")
agent.addParams({
    "initial_command": "0",
    "max_iterations": "6",
    "verbose": "true",
})

# Wire: CPU → Hali → dTLB (Hali intercepts MMIO, passes everything else)
link_cpu_hali = sst.Link(prefix + ".link_cpu_hali")
link_cpu_hali.connect((cpuDcacheIf, "lowlink", "1ns"), (hali, "highlink", "1ns"))
link_cpu_hali.setNoCut()

link_hali_dtlb = sst.Link(prefix + ".link_hali_dtlb")
link_hali_dtlb.connect((hali, "lowlink", "1ns"), (dtlbWrapper, "cpu_if", "1ns"))
link_hali_dtlb.setNoCut()
```

The `intercept_ranges` parameter (`"0xBEEF0000,4096"`) tells Hali which
addresses to intercept. This **must match** the `HYADES_MMIO_BASE` used in the
C binary (default `0xBEEF0000`).

---




