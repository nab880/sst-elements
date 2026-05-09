// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef SST_ELEMENTS_CARCOSA_ECC_SCHEME_H
#define SST_ELEMENTS_CARCOSA_ECC_SCHEME_H

#include <cstdint>
#include <string>

namespace SST {
namespace Carcosa {

enum class EccOutcome : uint8_t {
    Clean                    = 0,
    Correctable              = 1,
    DetectableUncorrectable  = 2,
    SilentEscape             = 3,
};

inline const char* eccOutcomeName(EccOutcome o) {
    switch (o) {
    case EccOutcome::Clean:                   return "clean";
    case EccOutcome::Correctable:             return "correctable";
    case EccOutcome::DetectableUncorrectable: return "due";
    case EccOutcome::SilentEscape:            return "escape";
    }
    return "unknown";
}

enum class EccScheme : uint8_t {
    NONE        = 0,
    SECDED_64   = 1,
    CHIPKILL_x4 = 2,
};

inline const char* eccSchemeName(EccScheme s) {
    switch (s) {
    case EccScheme::NONE:        return "none";
    case EccScheme::SECDED_64:   return "secded";
    case EccScheme::CHIPKILL_x4: return "chipkill";
    }
    return "unknown";
}

inline bool eccSchemeFromString(const std::string& s, EccScheme& out) {
    if (s == "none" || s == "NONE")           { out = EccScheme::NONE;        return true; }
    if (s == "secded" || s == "SECDED" ||
        s == "secded_64" || s == "SECDED_64") { out = EccScheme::SECDED_64;   return true; }
    if (s == "chipkill" || s == "CHIPKILL" ||
        s == "chipkill_x4")                   { out = EccScheme::CHIPKILL_x4; return true; }
    return false;
}

// Conservative aggregation: treats total errors as if landing in one protection
// word. Overestimates ECC failures vs per-word draw; defensible upper bound for
// the low-BER regime (errors-per-access << 1) targeted by the experiment.
inline EccOutcome classifyEccOutcome(unsigned num_bit_errors,
                                     uint32_t /*payload_bytes*/,
                                     EccScheme scheme) {
    if (num_bit_errors == 0) return EccOutcome::Clean;

    switch (scheme) {
    case EccScheme::NONE:
        return EccOutcome::SilentEscape;

    case EccScheme::SECDED_64:
        if (num_bit_errors == 1) return EccOutcome::Correctable;
        if (num_bit_errors == 2) return EccOutcome::DetectableUncorrectable;
        return EccOutcome::SilentEscape;

    case EccScheme::CHIPKILL_x4:
        if (num_bit_errors <= 4) return EccOutcome::Correctable;
        if (num_bit_errors <= 8) return EccOutcome::DetectableUncorrectable;
        return EccOutcome::SilentEscape;
    }
    return EccOutcome::SilentEscape;
}

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_SCHEME_H */
