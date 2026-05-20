// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

#include "insn_classifier.h"

#include "decoder_aarch64.h"
#include "decoder_generic.h"
#include "decoder_riscv.h"
#include "plugin_state.h"

using namespace SST::Quetz;

QuetzInsnClass InsnClassifier::refineMemClass(QuetzInsnClass cls,
                                              uint32_t /*size*/) const {
    return cls;
}

QuetzInsnClass RiscvInsnClassifier::classify(uint32_t enc) const {
    return classify_riscv_insn(enc);
}

QuetzInsnClass Aarch64InsnClassifier::classify(uint32_t enc) const {
    return classify_aarch64_insn(enc);
}

QuetzInsnClass GenericInsnClassifier::refineMemClass(QuetzInsnClass /*cls*/,
                                                     uint32_t size) const {
    return classify_by_size(size);
}

static RiscvInsnClassifier   s_riscv_classifier;
static Aarch64InsnClassifier s_aarch64_classifier;
static GenericInsnClassifier s_generic_classifier;

InsnClassifier* create_insn_classifier(QuetzISA isa) {
    switch (isa) {
    case QUETZ_ISA_RISCV:   return &s_riscv_classifier;
    case QUETZ_ISA_AARCH64: return &s_aarch64_classifier;
    default:                return &s_generic_classifier;
    }
}
