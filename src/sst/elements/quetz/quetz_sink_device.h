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

#ifndef _H_SST_QUETZ_SINK_DEVICE
#define _H_SST_QUETZ_SINK_DEVICE

#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>

#include <stdint.h>
#include <string>
#include <vector>

namespace SST {
namespace Quetz {

// Write-side mirror of QuetzStreamDevice: the guest pushes actuator commands /
// telemetry / processed output through a tiny register interface and SST
// captures the bytes to a file for host-side assertion — closing the
// stimulus -> compute -> captured-output loop that recorded-input replay
// (stream device) opens. Purely reactive — no clock, no timing model —
// matching the "prove the code works" fidelity level of the quetz functional
// decks. Stream + sink together are the documented copy-one-of-these-two
// peripheral templates.
class QuetzSinkDevice : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        QuetzSinkDevice,
        "quetz",
        "QuetzSinkDevice",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "File-capturing actuator/telemetry sink MMIO device (functional, no timing)",
        COMPONENT_CATEGORY_MEMORY)

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose", "(uint) Verbosity level", "0" },
        { "clock", "(UnitAlgebra/string) Clock used for the MMIO interface", "1GHz" },
        { "base_addr", "(uint64) MMIO region base address", "0" },
        { "mmio_size", "(uint64) Size of MMIO region in bytes", "0x100" },
        { "sink_file",
          "(string) File the captured bytes are written to (required); "
          "written on CTRL flush and unconditionally at finish()", "" },
        { "max_bytes",
          "(uint64) Capture cap: bytes pushed beyond it are dropped and "
          "counted in dropped_bytes (protects CI disks). 0 = unlimited.", "0" })

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "iface", "MMIO target interface (sink registers)",
          "SST::Interfaces::StandardMem" })

    SST_ELI_DOCUMENT_STATISTICS(
        { "bytes_accepted", "Bytes pushed through DATA and captured", "bytes", 1 },
        { "flushes", "CTRL flush commands", "requests", 1 },
        { "truncates", "CTRL truncate/restart commands", "requests", 1 },
        { "dropped_bytes", "Bytes dropped beyond the max_bytes cap", "bytes", 1 },
        { "wrong_direction_accesses",
          "Reads/writes to mapped registers with the wrong direction", "requests", 1 },
        { "bad_offset_accesses",
          "Reads/writes to offsets not in the register map", "requests", 1 })

    QuetzSinkDevice(ComponentId_t id, Params& params);

    void init(unsigned int phase) override;
    void setup() override;
    void finish() override;

    // Register map (8-byte stride, same conventions as QuetzStreamDevice so
    // firmware idioms transfer; 32-bit guest accesses see the low 4 bytes).
    //
    // DATA pushes exactly write-size bytes: an 8/16/32-bit store pushes
    // 1/2/4 bytes, unpacked from the value low-byte-first (the numeric
    // mirror of the stream device's pop packing, so it is endian-agnostic —
    // identical firmware runs on ColdFire (BE) and RISC-V (LE)). The store
    // size being explicit on the write path is what handles trailing partial
    // words with no extra register.
    static constexpr uint64_t REG_STATUS = 0x00;  // R: bytes accepted so far (== SEQ)
    static constexpr uint64_t REG_DATA   = 0x08;  // W: push write-size bytes
    static constexpr uint64_t REG_SEQ    = 0x10;  // R: bytes accepted so far
    static constexpr uint64_t REG_CTRL   = 0x18;  // W: 1 = flush to file; 2 = truncate/restart

protected:
    ~QuetzSinkDevice() {}

    void handleEvent(Interfaces::StandardMem::Request* req);
    void writeFile(bool truncate_only);

    class mmioHandlers : public Interfaces::StandardMem::RequestHandler {
    public:
        friend class QuetzSinkDevice;

        mmioHandlers(QuetzSinkDevice* dev, SST::Output* out)
            : Interfaces::StandardMem::RequestHandler(out), dev(dev)
        {}
        virtual ~mmioHandlers() {}
        virtual void handle(Interfaces::StandardMem::Read* read) override;
        virtual void handle(Interfaces::StandardMem::Write* write) override;

        QuetzSinkDevice* dev;
    };

    void printStatus(Output& out) override;
    void emergencyShutdown() override {}

    Output out;

    uint64_t base_addr_;
    uint64_t mmio_size_;
    uint64_t max_bytes_;

    std::string          sink_file_;
    std::vector<uint8_t> captured_;
    size_t                flushed_bytes_; // prefix already persisted to sink_file_
    uint64_t             accepted_;   // bytes accepted (== captured_.size())
    uint64_t             dropped_;

    mmioHandlers* handlers_;
    Interfaces::StandardMem* iface_;

    Statistic<uint64_t>* stat_bytes_accepted_;
    Statistic<uint64_t>* stat_flushes_;
    Statistic<uint64_t>* stat_truncates_;
    Statistic<uint64_t>* stat_dropped_bytes_;
    Statistic<uint64_t>* stat_wrong_direction_accesses_;
    Statistic<uint64_t>* stat_bad_offset_accesses_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_SINK_DEVICE
