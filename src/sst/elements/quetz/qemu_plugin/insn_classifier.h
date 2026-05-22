// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

#ifndef _QUETZ_INSN_CLASSIFIER_H
#define _QUETZ_INSN_CLASSIFIER_H

#include "../quetz_ipc_types.h"

#include <cstdint>

namespace SST {
namespace Quetz {

class InsnClassifier {
public:
    virtual ~InsnClassifier() = default;

    virtual QuetzInsnClass classify(uint32_t enc) const = 0;

    virtual bool usesPreciseMemCallbacks() const = 0;

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

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_INSN_CLASSIFIER_H
