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

#include <deque>
#include <stdint.h>
#include <unordered_map>
#include <vector>

#include "quetz_kernel_api.h"

namespace SST {
namespace Quetz {


class QuetzGpuDevice : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        QuetzGpuDevice,
        "quetz",
        "QuetzGpuDevice",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Synthetic accelerator MMIO device: doorbell latency model, or real "
        "compute via a QuetzKernel in the 'kernel' slot",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose", "(uint) Verbosity level", "0" },
        { "clock", "(UnitAlgebra/string) Clock frequency", "1GHz" },
        { "base_addr", "(uint64) MMIO region base address", "0" },
        { "mmio_size", "(uint64) Size of MMIO region in bytes", "0x400" },
        { "kernel_latency", "(uint64) Default kernel runtime in clock cycles "
          "(used when no kernel is loaded, or when the loaded kernel returns "
          "no latency opinion)", "5000" },
        { "doorbell_blocking",
          "(bool) If 1, defer the doorbell write response until the kernel "
          "retires (mimics balar's blocked_response). Lets the Quetz async "
          "engine's COMPLETED counter track real kernel completion. Default 0 "
          "(respond immediately; guest polls REG_STATUS). Required when the "
          "'kernel' slot is populated.", "0" },
        { "irq_line",
          "(int) Machine interrupt-controller input raised when an op "
          "retires. Level semantics with event counting: each retire adds one "
          "completion event; a REG_IRQ_ACK write of N consumes up to N events "
          "and the line lowers only when all events are consumed, so a "
          "retirement between IRQ delivery and ack is never lost. Requires "
          "the 'irq' port to be linked to a QuetzCPU irq_link_%d port. -1 = "
          "disabled (poll REG_STATUS/REG_KERNEL_ID instead).", "-1" },
        { "irq_vcpu",
          "(uint32) IRQ-mailbox row the raise is posted to (single-core "
          "guests: 0).", "0" },
        { "dma_bytes_per_kernel",
          "(uint64) Synthetic DMA bytes per kernel (P2.b only; must be 0)", "0" },
        { "dma_range_start",
          "(uint64) With dma_range_end: the only guest-phys range kernel DMA "
          "may touch (the SST-backed window). An op whose input or output "
          "buffer falls outside is REJECTED — counted in ops_rejected, the "
          "doorbell completes, kernel_id does not advance — instead of the "
          "guest-programmed address crashing the simulation in memHierarchy "
          "routing. Both 0 = unrestricted (legacy behavior).", "0" },
        { "dma_range_end",
          "(uint64) Inclusive end of the kernel-DMA range; see "
          "dma_range_start. 0 = unrestricted (setting dma_range_start "
          "without dma_range_end is a fatal config error).", "0" },
        { "data_big_endian",
          "(bool) Byte layout of the kernel's DMA'd buffers: interpret words "
          "MSB-first (big-endian) instead of little-endian. Pushed into the "
          "'kernel' subcomponent at load — kernels take no endianness param "
          "of their own. Set alongside the QuetzCPU's window_big_endian=1 "
          "when the buffers live in a BE-packed SST window. Default 0 (LE).",
          "0" })

    SST_ELI_DOCUMENT_PORTS(
        { "irq",
          "Optional IRQ-injection link to a QuetzCPU irq_link_%d port "
          "(quetz.QuetzIrqEvent); used when irq_line >= 0.",
          { "quetz.QuetzIrqEvent" } })

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "iface", "MMIO target interface (doorbell/status registers)",
          "SST::Interfaces::StandardMem" },
        { "mem_iface", "Memory initiator for kernel DMA (required when the "
          "'kernel' slot is populated)",
          "SST::Interfaces::StandardMem" },
        { "kernel", "Compute kernel run per doorbell: the device DMA-reads "
          "inputBytes() from REG_ARG0, calls compute(), stays BUSY for the "
          "returned latency, then DMA-writes the output to REG_ARG1. Slot "
          "empty = pure latency model.",
          "SST::Quetz::QuetzKernel" })

    SST_ELI_DOCUMENT_STATISTICS(
        { "kernels_launched", "Doorbell writes that started a kernel", "kernels", 1 },
        { "busy_cycles", "Cycles spent in BUSY state", "cycles", 1 },
        { "doorbell_writes", "Writes to the doorbell register", "requests", 1 },
        { "status_polls", "Reads of the status register", "requests", 1 },
        { "latency_overrides", "Writes to the latency-override register", "requests", 1 },
        { "doorbell_while_busy",
          "Doorbell writes while BUSY (queued or dropped if queue full)", "requests", 1 },
        { "irqs_raised",
          "Completion IRQs raised (0->1 line transitions; irq_line >= 0 only)",
          "interrupts", 1 },
        { "wrong_direction_accesses",
          "Reads/writes to mapped registers with the wrong direction", "requests", 1 },
        { "bad_offset_accesses",
          "Reads/writes to offsets not in the register map", "requests", 1 },
        { "ops_rejected",
          "Kernel ops rejected non-fatally (kernel refused the args, or a "
          "buffer escapes dma_range_start/end); kernel_id does not advance",
          "operations", 1 })

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
        // Kernel DMA (mem_iface) responses.
        virtual void handle(Interfaces::StandardMem::ReadResp* resp) override;
        virtual void handle(Interfaces::StandardMem::WriteResp* resp) override;

        static void u64ToData(uint64_t val, std::vector<uint8_t>* data, size_t size);
        static uint64_t dataToU64(std::vector<uint8_t>* data);

        QuetzGpuDevice* gpu;
    };

    void printStatus(Output& out) override;
    void emergencyShutdown() override {}

    bool tickBusy(SST::Cycle_t cycle);
    void retireIfReady(uint64_t now_clk);
    bool isBusyAt(uint64_t now_clk) const;
    bool hasOutstandingWork() const;
    void startKernel(uint64_t now_clk, uint64_t latency);
    void updatePrimaryHold(bool allow_ok_to_end);
    void raiseIrqOnRetire();
    void ackIrq(uint64_t consume);

    // --- kernel-slot op: DMA state machine around the plugged compute ---------
    void opStartDma();            // on doorbell: begin DMA-read of the input
    void opIssueReadWindow();     // keep a bounded set of reads in flight
    void opOnReadResp(Interfaces::StandardMem::ReadResp* resp);
    void opIssueWriteWindow();    // keep a bounded set of writes in flight
    void opOnWriteResp(Interfaces::StandardMem::WriteResp* resp);
    void opComputeAndStartBusy(); // input fully read: compute, then go BUSY
    void opBeginWriteback();      // busy done: DMA-write the result
    void opFinish();              // writeback done: retire + release doorbell
    void opReject(const char* why);  // abandon the op non-fatally
    bool dmaRangeOk(uint64_t addr, uint64_t len) const;
    Output out;

    TimeConverter tc_;
    uint64_t gpu_clk_;
    uint64_t base_addr_;
    uint64_t mmio_size_;
    uint64_t kernel_latency_;
    uint64_t busy_until_clk_;
    uint64_t kernel_id_;
    uint64_t submit_id_;
    uint64_t latency_override_;
    bool holding_sim_;
    bool doorbell_blocking_;
    // Held doorbell write response (doorbell_blocking_ mode): sent when the
    // kernel it launched retires, so the requester sees completion only then.
    Interfaces::StandardMem::Request* deferred_doorbell_resp_;
    std::deque<uint64_t> pending_latencies_;
    mmioHandlers* handlers;
    Interfaces::StandardMem* iface;

    // --- completion IRQ (irq_line_ >= 0) ---------------------------------------
    int64_t   irq_line_;
    uint32_t  irq_vcpu_;
    bool      irq_pending_;      // line currently raised
    uint64_t  irq_events_;       // completions not yet consumed by an ack
    SST::Link* irq_link_;

    // --- kernel-slot state -----------------------------------------------------
    QuetzKernel* kernel_;                  // nullptr = pure latency model
    Interfaces::StandardMem* mem_iface_;   // memory initiator (kernel ops)
    // Guest-phys range kernel DMA may touch (dma_range_end_ == 0 = any).
    uint64_t dma_range_start_;
    uint64_t dma_range_end_;

    // Guest-programmed operand registers (REG_ARG0..3), captured into a
    // KernelArgs at doorbell time.
    uint64_t arg_regs_[4];
    KernelArgs op_args_;

    // DMA phases of one kernel op.
    enum class OpPhase { IDLE, READING, WRITING };
    OpPhase op_phase_;
    std::vector<uint8_t> op_in_;                     // DMA-read input bytes
    std::vector<uint8_t> op_out_;                    // kernel output bytes
    uint64_t op_in_bytes_;
    uint64_t op_dma_outstanding_;                    // in-flight mem requests
    uint64_t op_next_dma_off_;                       // next byte not yet issued
    // held doorbell response for the whole op (released at opFinish)
    Interfaces::StandardMem::Request* op_doorbell_resp_;
    // map mem request id -> byte offset into op_in_ (for read reassembly)
    std::unordered_map<Interfaces::StandardMem::Request::id_t, uint64_t> op_req_off_;

    static constexpr uint64_t kOpDmaChunk = 64;      // bytes per DMA request
    static constexpr uint64_t kMaxOpDmaOutstanding = 64;

    Statistic<uint64_t>* stat_kernels_launched_;
    Statistic<uint64_t>* stat_busy_cycles_;
    Statistic<uint64_t>* stat_doorbell_writes_;
    Statistic<uint64_t>* stat_status_polls_;
    Statistic<uint64_t>* stat_latency_overrides_;
    Statistic<uint64_t>* stat_doorbell_while_busy_;
    Statistic<uint64_t>* stat_irqs_raised_;
    Statistic<uint64_t>* stat_wrong_direction_accesses_;
    Statistic<uint64_t>* stat_bad_offset_accesses_;
    Statistic<uint64_t>* stat_ops_rejected_;

    static constexpr size_t kMaxPendingLaunches = 8;
    static constexpr uint64_t REG_DOORBELL         = 0x00;  // W: submit
    static constexpr uint64_t REG_STATUS           = 0x08;  // R: busy(1)/idle(0)
    static constexpr uint64_t REG_KERNEL_ID        = 0x10;  // R: completed-ticket counter
    static constexpr uint64_t REG_LATENCY_OVERRIDE   = 0x18;  // W
    // P4 async poll contract: ticket of most recent submit, and the result
    // latch of the last completed op. The guest waits with
    // `while (REG_KERNEL_ID < my_ticket);` after reading its ticket here.
    static constexpr uint64_t REG_TICKET           = 0x20;  // R: last submit ticket
    static constexpr uint64_t REG_RESULT           = 0x28;  // R: last completed result
    // Kernel operand registers: the guest programs these before the doorbell;
    // meaning is kernel-defined (see KernelArgs in quetz_kernel_api.h).
    static constexpr uint64_t REG_ARG0             = 0x30;  // W: input buffer addr
    static constexpr uint64_t REG_ARG1             = 0x38;  // W: output buffer addr
    static constexpr uint64_t REG_ARG2             = 0x40;  // W: kernel-defined (FFT: N)
    static constexpr uint64_t REG_ARG3             = 0x48;  // W: kernel-defined
    // Completion IRQ (irq_line >= 0): R = 1 while the line is raised;
    // W nonzero = ack (lowers the line). See SIMULATING-YOUR-SYSTEM.md.
    static constexpr uint64_t REG_IRQ_ACK          = 0x50;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_GPU_DEVICE
