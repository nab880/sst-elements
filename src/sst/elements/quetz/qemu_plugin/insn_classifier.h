// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

/**
 * insn_classifier.h — strategy interface for guest instruction classification.
 *
 * Each ISA backend (RISC-V, AArch64, generic size-based fallback) implements
 * InsnClassifier so instrument.cpp stays free of per-ISA conditionals.
 */

#ifndef _QUETZ_INSN_CLASSIFIER_H
#define _QUETZ_INSN_CLASSIFIER_H

#include "plugin_state.h"

#include <cstdint>

namespace SST {
namespace Quetz {

class InsnClassifier {
public:
    virtual ~InsnClassifier() = default;

    /** Classify a guest instruction from its 32-bit encoding. */
    virtual QuetzInsnClass classify(uint32_t enc) const = 0;

    /**
     * When true, mem and exec callbacks are registered separately at TB
     * translation time (precise mode).  When false, both callbacks are
     * registered and classification is deferred to the mem callback.
     */
    virtual bool usesPreciseMemCallbacks() const = 0;

    /** Refine the memory-access class at runtime (generic ISA uses size). */
    virtual QuetzInsnClass refineMemClass(QuetzInsnClass cls,
                                          uint32_t size) const;
};

class RiscvInsnClassifier : public InsnClassifier {
public:
    QuetzInsnClass classify(uint32_t enc) const override;
    bool usesPreciseMemCallbacks() const override { return true; }
};

class Aarch64InsnClassifier : public InsnClassifier {
public:
    QuetzInsnClass classify(uint32_t enc) const override;
    bool usesPreciseMemCallbacks() const override { return true; }
};

class GenericInsnClassifier : public InsnClassifier {
public:
    QuetzInsnClass classify(uint32_t /*enc*/) const override {
        return QUETZ_INSN_OTHER;
    }
    bool usesPreciseMemCallbacks() const override { return false; }
    QuetzInsnClass refineMemClass(QuetzInsnClass /*cls*/,
                                  uint32_t size) const override;
};

InsnClassifier* create_insn_classifier(QuetzISA isa);

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_INSN_CLASSIFIER_H
