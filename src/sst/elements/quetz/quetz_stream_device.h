// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _H_SST_QUETZ_STREAM_DEVICE
#define _H_SST_QUETZ_STREAM_DEVICE

#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>

#include <stdint.h>
#include <vector>

namespace SST {
namespace Quetz {

// File-backed data-feed MMIO device: replays a fixture file (sensor samples,
// GPS/telemetry logs, ADC captures, ...) to the guest through a tiny FIFO
// register interface, so embedded code can be functionally validated against
// recorded device data without the physical peripheral. Purely reactive — no
// clock-driven behavior, no timing model — matching the "prove the code works"
// fidelity level of the quetz functional decks.
class QuetzStreamDevice : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        QuetzStreamDevice,
        "quetz",
        "QuetzStreamDevice",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "File-backed sensor/data stream MMIO device (functional, no timing)",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose", "(uint) Verbosity level", "0" },
        { "clock", "(UnitAlgebra/string) Clock used for the MMIO interface", "1GHz" },
        { "base_addr", "(uint64) MMIO region base address", "0" },
        { "mmio_size", "(uint64) Size of MMIO region in bytes", "0x100" },
        { "stream_file",
          "(string) Binary fixture file replayed through REG_DATA (required)", "" },
        { "pace_bytes",
          "(uint64) Pacing: bytes made available per pace_period (0 = whole "
          "stream available at t=0, today's behavior). With pacing on, STATUS "
          "reports bytes ready NOW and firmware must poll (STATUS==0 && "
          "REG_EOS==0 means 'not ready yet').", "0" },
        { "pace_period",
          "(UnitAlgebra/string) Refill interval for pace_bytes", "100us" },
        { "irq_line",
          "(int) Machine interrupt-controller input asserted while paced data "
          "is available. ACK leaves it high while STATUS > 0; draining the "
          "last available bytes lowers it. Requires pacing "
          "(pace_bytes > 0) and the 'irq' port linked to a QuetzCPU "
          "irq_link_%d port. -1 = disabled.", "-1" },
        { "irq_vcpu",
          "(uint32) IRQ-mailbox row the raise is posted to (single-core "
          "guests: 0).", "0" })

    SST_ELI_DOCUMENT_PORTS(
        { "irq",
          "Optional IRQ-injection link to a QuetzCPU irq_link_%d port "
          "(quetz.QuetzIrqEvent); used when irq_line >= 0.",
          { "quetz.QuetzIrqEvent" } })

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "iface", "MMIO target interface (stream registers)",
          "SST::Interfaces::StandardMem" })

    SST_ELI_DOCUMENT_STATISTICS(
        { "data_reads", "Reads of the DATA register", "requests", 1 },
        { "bytes_delivered", "Stream bytes handed to the guest", "bytes", 1 },
        { "status_polls", "Reads of the STATUS register", "requests", 1 },
        { "underruns", "DATA reads with the stream exhausted", "requests", 1 },
        { "not_ready_reads",
          "DATA reads before the paced refill made the bytes available", "requests", 1 },
        { "paced_refills", "Pacing ticks that made new bytes available", "ticks", 1 },
        { "rewinds", "CTRL rewind commands", "requests", 1 },
        { "irqs_raised",
          "Data-ready IRQs raised (0->1 line transitions; irq_line >= 0 only)",
          "interrupts", 1 },
        { "wrong_direction_accesses",
          "Reads/writes to mapped registers with the wrong direction", "requests", 1 },
        { "bad_offset_accesses",
          "Reads/writes to offsets not in the register map", "requests", 1 })

    QuetzStreamDevice(ComponentId_t id, Params& params);

    void init(unsigned int phase) override;
    void setup() override;

    // Register map (8-byte stride; 32-bit guest reads see the low 4 bytes,
    // matching the QuetzGpuDevice convention for 32-bit cores like ColdFire).
    //
    // DATA pops up to 4 stream bytes packed *numerically*:
    //   value = b[i] | b[i+1]<<8 | b[i+2]<<16 | b[i+3]<<24
    // The guest extracts bytes with shifts/masks, so the packing is
    // endian-agnostic — identical firmware runs on ColdFire (BE) and RISC-V (LE).
    static constexpr uint64_t REG_STATUS = 0x00;  // R: bytes ready now (unpaced: all remaining)
    static constexpr uint64_t REG_DATA   = 0x08;  // R: pop up to 4 bytes (packed)
    static constexpr uint64_t REG_SEQ    = 0x10;  // R: bytes consumed so far
    static constexpr uint64_t REG_CTRL   = 0x18;  // W: 1 = rewind to start
    static constexpr uint64_t REG_EOS    = 0x20;  // R: 1 = stream fully consumed
    // Data-ready IRQ (irq_line >= 0): R = 1 while the line is raised;
    // W nonzero = ack (line stays high while STATUS > 0).
    static constexpr uint64_t REG_IRQ_ACK = 0x28;

protected:
    ~QuetzStreamDevice() {}

    void handleEvent(Interfaces::StandardMem::Request* req);

    class mmioHandlers : public Interfaces::StandardMem::RequestHandler {
    public:
        friend class QuetzStreamDevice;

        mmioHandlers(QuetzStreamDevice* dev, SST::Output* out)
            : Interfaces::StandardMem::RequestHandler(out), dev(dev)
        {}
        virtual ~mmioHandlers() {}
        virtual void handle(Interfaces::StandardMem::Read* read) override;
        virtual void handle(Interfaces::StandardMem::Write* write) override;

        QuetzStreamDevice* dev;
    };

    void printStatus(Output& out) override;
    void emergencyShutdown() override {}

    bool tickPace(SST::Cycle_t cycle);
    void raiseDataReadyIrq();
    void ackIrq();

    Output out;

    uint64_t base_addr_;
    uint64_t mmio_size_;

    std::vector<uint8_t> stream_;
    size_t               pos_;

    // Pacing (pace_bytes_ == 0 => whole stream available immediately).
    uint64_t pace_bytes_;
    uint64_t budget_given_;   // cumulative bytes made available
    uint64_t avail_;          // bytes available and not yet popped

    // Data-ready IRQ (irq_line_ >= 0, paced only).
    int64_t    irq_line_;
    uint32_t   irq_vcpu_;
    bool       irq_pending_;
    SST::Link* irq_link_;

    mmioHandlers* handlers_;
    Interfaces::StandardMem* iface_;

    Statistic<uint64_t>* stat_data_reads_;
    Statistic<uint64_t>* stat_bytes_delivered_;
    Statistic<uint64_t>* stat_status_polls_;
    Statistic<uint64_t>* stat_underruns_;
    Statistic<uint64_t>* stat_not_ready_reads_;
    Statistic<uint64_t>* stat_paced_refills_;
    Statistic<uint64_t>* stat_rewinds_;
    Statistic<uint64_t>* stat_irqs_raised_;
    Statistic<uint64_t>* stat_wrong_direction_accesses_;
    Statistic<uint64_t>* stat_bad_offset_accesses_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_STREAM_DEVICE
