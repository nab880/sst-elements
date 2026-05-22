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
