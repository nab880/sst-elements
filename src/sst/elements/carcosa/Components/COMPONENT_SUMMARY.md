# Carcosa Components – Summary

Short description of each component in `carcosa/Components/` and how it is implemented.

---

## Hali

**Role:** Interface layer between sensors/CPUs and the memory hierarchy in vehicle-style simulations.

**Behavior:**  
Hali sits between CPUs, optional SensorComponents, optional CarcosaMemCtrl, and the memHierarchy (highlink = CPU/cache side, lowlink = memory side). It supports a **ring topology** with other Hali instances via `left`/`right` links. In **init**, it discovers ring neighbors by sending/receiving untimed `HaliEvent`s. In **setup**, it computes how many events to send per neighbor and sends the first timed `HaliEvent`. During **run**, it: (1) receives `HaliEvent`s—if for self, counts and may send the next event to a neighbor; if for another, forwards on the ring; (2) receives `CpuEvent`/`FaultInjEvent` on `cpu`—counts, optionally queues PM commands in `FaultInjManager`, and deletes; (3) receives `SensorEvent` on `sensor`—on last event calls `primaryComponentOKToEndSim()`, otherwise sends a `CpuEvent` to `cpu`; (4) forwards MemHierarchy events between `highlink` and `lowlink`, with `FaultInjManager` optionally attaching PM data to highlink→lowlink `MemEvent`s; (5) forwards `CpuEvent`s from `memCtrl` to the optional `memCtrl` link. **Complete** forwards untimed MemHierarchy traffic and exchanges goodbye/farewell `HaliEvent`s on the ring. **Finish** prints sent/received/forwarded counts.

**Implementation:**  
SST `Component` with `requireLibrary("memHierarchy")`. Uses `Event::Handler2` for all links; optional links (`sensor`, `highlink`, `lowlink`, `memCtrl`) are only configured when the port is connected. Loads a `FaultInjManagerAPI` subcomponent (`Carcosa.FaultInjManager`) to queue and attach PM requests to memory traffic. Primary component; calls `primaryComponentOKToEndSim()` when ring or sensor workload is done.

---

## CarcosaCPU

**Role:** Simple demo CPU that issues StandardMem traffic and can be driven by Hali via `Carcosa.CpuEvent`s.

**Behavior:**  
Uses a **clock** and a **StandardMem** subcomponent (e.g. `memHierarchy.standardInterface`) to issue reads, writes, flushes, flush-inv, custom, LL/SC, and optional MMIO. Parameters define `memSize`, `opCount`, `memFreq`, and frequency mix (read_freq, write_freq, etc.). On the **haliToCPU** link it only **receives** `CpuEvent`s (e.g. from Hali when a sensor ticks); it handles them in `handleCpuEvent` and deletes. It does not send fault-injection or PM events; it just runs memory ops until `opCount` is reached and then calls `primaryComponentOKToEndSim()`.

**Implementation:**  
Lives in `SST::MemHierarchy`, registered as `Carcosa.CarcosaCPU`. Uses `Clock::Handler2` for the tick and `Event::Handler2` for `haliToCPU`; StandardMem is loaded in the `memory` slot with `StandardMem::Handler2` for responses. Statistics (reads, writes, flushes, etc.) are registered per ELI. Primary component.

---

## FaultInjCPU

**Role:** Demo CPU that both issues StandardMem traffic and sends fault-injection/PM traffic to Hali.

**Behavior:**  
Same memory-issuing behavior as CarcosaCPU (clock + StandardMem, same params). Additionally: (1) **Sends** `FaultInjEvent`s to Hali on **haliToCPU** on a schedule (e.g. every 100 cycles: `injection_rate`, `set_range`, `config`, `disable_faults`) and (2) periodically sends `CpuEvent`s with file/start/end (e.g. `test1.txt`, 0, 200) to Hali, which can be forwarded to CarcosaMemCtrl for BackingHybrid region setup. On **haliToCPU** it **receives** only `CpuEvent`s (from Hali); `handleCpuEvent` deletes them. So the link is bidirectional in usage: CPU → Hali for FaultInj/Cpu events, Hali → CPU for CpuEvents.

**Implementation:**  
Same structure as CarcosaCPU (namespace `SST::MemHierarchy`, `Carcosa.FaultInjCPU`), with an extra `currentFaultRate` and logic in `clockTic` to create and send `FaultInjEvent`s and file-range `CpuEvent`s over `HaliLink`. Primary component.

---

## CarcosaMemCtrl

**Role:** Memory controller that talks to memHierarchy (caches/CPUs) and optionally to Hali for dynamic backing regions.

**Behavior:**  
Extends the usual memHierarchy memory-controller behavior (highlink, backend, backing store, optional listeners/custom commands). **Extra:** up to `numIFLs` links named `iflLinks_0`, `iflLinks_1`, … that accept **Carcosa.CpuEvent**s. When such an event is received, it is interpreted as a request to add a backing file range: `fname`, `start`, `end`. That is only applied when the backing store is **BackingHybrid** (`backing=hybrid`). If backing is not hybrid, the event is ignored (and the event is still deleted). So for IFL/PM-style dynamic regions, config must use `backing=hybrid` and connect Hali (or another source) to `iflLinks_N`.

**Implementation:**  
SST `Component` in `SST::MemHierarchy`, registered as `Carcosa.CarcosaMemCtrl`. Uses `Clock::Handler2` for the controller clock and `Event::Handler2` for highlink and for each `iflLinks_N`. Backing can be `none`, `mmap`, `malloc`, or `hybrid`; only `hybrid` supports `addBacking()` from IFL events. Requires `memHierarchy` library.

---

## FaultInjManagerAPI / FaultInjManager

**Role:** Subcomponent API and implementation for queuing PM/fault-injection requests and attaching them to MemEvents by ID.

**Behavior:**  
**FaultInjManagerAPI** is an abstract subcomponent interface: `processHighLinkMessage(MemEvent*)`, `processLowLinkMessage(MemEvent*)`, `addHighLinkRequest(string)`, `addLowLinkRequest(string)`. **FaultInjManager** implements it with two queues (`pendingHighLinkRequests`, `pendingLowLinkRequests`). When a MemEvent is processed (high or low), if the corresponding queue is non-empty, the front string is registered in **PMDataRegistry** under that event’s ID and the queue is popped; the same MemEvent pointer is returned (PM data is stored by ID elsewhere). No event creation or modification; the backing/fault injector uses the registry to interpret PM data for a given event ID.

**Implementation:**  
`FaultInjManagerAPI` is an `SST::SubComponent` with `SST_ELI_REGISTER_SUBCOMPONENT_API`. `FaultInjManager` is registered as implementing that API via `SST_ELI_REGISTER_SUBCOMPONENT` and is typically loaded anonymously by Hali (e.g. `Carcosa.FaultInjManager` in the `faultInjManager` slot). Serialization hooks are present but currently no-op.

---

## SensorComponent

**Role:** Mimics a sensor that periodically sends data to an “IFL” (interface layer), implemented as Hali.

**Behavior:**  
Sends a fixed number of timed **SensorEvent**s on the **ifl** link (e.g. “tick” messages from a 1 Hz clock). When the configured count is reached, it sends a final **SensorEvent** with `setLast()` and calls `primaryComponentOKToEndSim()`. In **init** it sends untimed `SensorEvent(getName())` for discovery; in **complete** it sends a goodbye and receives a reply, stored for **finish** output. So the ifl link is intended to connect to Hali’s **sensor** port; Hali then forwards to CPUs via `CpuEvent`s and drives completion.

**Implementation:**  
SST `Component` in `SST::Carcosa`, registered as `Carcosa.SensorComponent`. Uses `Event::Handler2` for the ifl link and `Clock::Handler2` for the 1 Hz tick. Port documents `Carcosa.SensorEvent`. Primary component.

---

## Event types (CpuEvent, HaliEvent, FaultInjEvent, SensorEvent)

- **CpuEvent:** Carries string, number, and optional (fname, startAddr, endAddr). Used for data/commands from Hali to CPU, and from CPU/Hali to CarcosaMemCtrl for backing region (fname/start/end).
- **HaliEvent:** Carries string and number. Used for ring discovery and round-robin messaging between Hali components.
- **FaultInjEvent:** Carries fname, probability, rate, plus string/num. Used by FaultInjCPU to send PM/fault-injection commands to Hali (e.g. injection_rate, set_range, config, disable_faults).
- **SensorEvent:** Carries string, number, and `last_` flag. Used by SensorComponent to send ticks and “end” to Hali; Hali uses `isLast()` to call `primaryComponentOKToEndSim()`.

All are `SST::Event` subclasses with `ImplementSerializable` / `ImplementVirtualSerializable`, clone, and optional `toString()` for debugging.

---

## Fixes applied in this pass

- **Hali:** Sensor port was documented but never configured; now `sensorLink_` is configured when `sensor` is connected. `handleMemCtrlEvent` now forwards `CpuEvent` to `memCtrlLink_` when present and deletes the event when not forwarded or when type is wrong. ELI: `sensor` port documents `Carcosa.SensorEvent`; `cpu` port documents `Carcosa.CpuEvent` and `Carcosa.FaultInjEvent`.
- **CarcosaMemCtrl:** `haliLinks_%d` port doc changed from `VTO.CpuEvent` to `Carcosa.CpuEvent`. `handleIFLEvent` now checks that backing is `BackingHybrid` before calling `addBacking`, deletes the received event in all paths, and avoids dereferencing a null backing.
- **SensorComponent:** ifl port doc changed from `VTO.IFLEvent` to `Carcosa.SensorEvent`.
- **CarcosaCPU / FaultInjCPU:** `haliToCPU` port doc changed from `Carcosa.HaliEvent` to `Carcosa.CpuEvent` (what they actually receive).
