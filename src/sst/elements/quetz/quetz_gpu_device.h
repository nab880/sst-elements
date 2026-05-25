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

#ifndef _H_SST_QUETZ_GPU_DEVICE
#define _H_SST_QUETZ_GPU_DEVICE

#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>

#include <stdint.h>
#include <vector>

namespace SST {
namespace Quetz {

/*
 * Lightweight synthetic GPU MMIO device for Quetz system-mode tests.
 * Doorbell write starts a configurable kernel-latency busy window
 * (kernel_latency is in GPU clock cycles at the device clock rate).
 * STATUS read returns BUSY/IDLE; KERNEL_ID counts completed kernels.
 * One pending launch may be queued when a doorbell arrives while BUSY;
 * a second doorbell while the queue is full is counted and dropped.
 */
class QuetzGpuDevice : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        QuetzGpuDevice,
        "quetz",
        "QuetzGpuDevice",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Synthetic GPU MMIO device with configurable kernel latency",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose", "(uint) Verbosity level", "0" },
        { "clock", "(UnitAlgebra/string) Clock frequency", "1GHz" },
        { "base_addr", "(uint64) MMIO region base address", "0" },
        { "mmio_size", "(uint64) Size of MMIO region in bytes", "0x400" },
        { "kernel_latency", "(uint64) Default kernel runtime in clock cycles", "5000" },
        { "dma_bytes_per_kernel",
          "(uint64) Synthetic DMA bytes per kernel (P2.b only; must be 0)", "0" })

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "iface", "Interface into memory subsystem", "SST::Interfaces::StandardMem" })

    SST_ELI_DOCUMENT_STATISTICS(
        { "kernels_launched", "Doorbell writes that started a kernel", "kernels", 1 },
        { "busy_cycles", "Cycles spent in BUSY state", "cycles", 1 },
        { "doorbell_writes", "Writes to the doorbell register", "requests", 1 },
        { "status_polls", "Reads of the status register", "requests", 1 },
        { "latency_overrides", "Writes to the latency-override register", "requests", 1 },
        { "doorbell_while_busy",
          "Doorbell writes while BUSY (queued or dropped if queue full)", "requests", 1 },
        { "wrong_direction_accesses",
          "Reads/writes to mapped registers with the wrong direction", "requests", 1 },
        { "bad_offset_accesses",
          "Reads/writes to offsets not in the register map", "requests", 1 })

    QuetzGpuDevice(ComponentId_t id, Params& params);

    void init(unsigned int phase) override;
    void setup() override;

protected:
    ~QuetzGpuDevice() {}

    void handleEvent(Interfaces::StandardMem::Request* req);

    class mmioHandlers : public Interfaces::StandardMem::RequestHandler {
    public:
        friend class QuetzGpuDevice;

        mmioHandlers(QuetzGpuDevice* gpu, SST::Output* out)
            : Interfaces::StandardMem::RequestHandler(out), gpu(gpu)
        {}
        virtual ~mmioHandlers() {}
        virtual void handle(Interfaces::StandardMem::Read* read) override;
        virtual void handle(Interfaces::StandardMem::Write* write) override;

        static void u64ToData(uint64_t val, std::vector<uint8_t>* data, size_t size);
        static uint64_t dataToU64(std::vector<uint8_t>* data);

        QuetzGpuDevice* gpu;
    };

    void printStatus(Output& out) override;
    void emergencyShutdown() override {}

    bool tickBusy(SST::Cycle_t cycle);
    void retireIfReady(uint64_t now_clk);
    bool isBusyAt(uint64_t now_clk) const;
    void ensureClockRunning();

    Output out;

    TimeConverter tc_;
    uint64_t base_addr_;
    uint64_t mmio_size_;
    uint64_t kernel_latency_;
    uint64_t busy_until_clk_;
    uint64_t kernel_id_;
    uint64_t latency_override_;
    uint64_t pending_latency_;
    bool     pending_valid_;

    mmioHandlers* handlers;
    Interfaces::StandardMem* iface;
    Clock::HandlerBase* clock_handler_;
    bool clock_registered_;

    Statistic<uint64_t>* stat_kernels_launched_;
    Statistic<uint64_t>* stat_busy_cycles_;
    Statistic<uint64_t>* stat_doorbell_writes_;
    Statistic<uint64_t>* stat_status_polls_;
    Statistic<uint64_t>* stat_latency_overrides_;
    Statistic<uint64_t>* stat_doorbell_while_busy_;
    Statistic<uint64_t>* stat_wrong_direction_accesses_;
    Statistic<uint64_t>* stat_bad_offset_accesses_;

    static constexpr uint64_t REG_DOORBELL         = 0x00;
    static constexpr uint64_t REG_STATUS           = 0x08;
    static constexpr uint64_t REG_KERNEL_ID        = 0x10;
    static constexpr uint64_t REG_LATENCY_OVERRIDE   = 0x18;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_GPU_DEVICE
