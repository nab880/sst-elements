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
#include <vector>

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

// =====================================================================
// PER-WORD ECC MODEL
// =====================================================================
//
// A cache line is partitioned into independent ECC protection words. The
// scheme determines the data-bytes-per-word footprint:
//
//   SECDED_64    -> 64-bit (8-byte) data words; corrects 1 bit, detects 2.
//   CHIPKILL_x4  -> 128-bit (16-byte) data words (8 x4 chips per word);
//                   corrects any single x4 chip; detects up to 2-chip damage.
//
// The line-level outcome is the *worst* per-word outcome (lattice max over
// Clean < Correctable < DUE < Escape). Only the bit-errors that land in an
// escaping word actually corrupt the line; correctable words contribute 0
// leaked bits, and DUE words are intercepted before reaching the consumer.
//
// MODEL VALIDITY ENVELOPE (read this before extending the BER sweep)
// ------------------------------------------------------------------
// The per-word draws assume an independent-bit Bernoulli channel: each bit
// in the word flips independently with probability BER. Per word that gives
// a Binomial(bits_per_word, BER) -> Poisson(lambda = bits_per_word*BER)
// distribution on the bit-error count. The Poisson approximation, and the
// downstream classification thresholds, are quantitatively tight as long as
//
//     lambda  ==  bits_per_word * BER   <<   1
//
// because the missing-mass in the Poisson is bounded by lambda (P[>=2 errors
// in word] <= lambda^2/2, P[>=3] <= lambda^3/6, ...).
//
// Worst-case word width is CHIPKILL_x4 (16 bytes + ~2 byte ECC = ~144 bits)
// followed by SECDED_64 (8 bytes + 1 byte ECC = ~72 bits). Picking the
// inclusive "<1% tail mass" bound gives:
//
//     BER  <=  kEccBerTightUpperBound  =  1.0e-3
//
// At BER = 1e-3:
//   - SECDED word:   lambda = 0.072,  P[>=2 errors] ~ 2.5e-3  (0.25%)
//   - Chipkill word: lambda = 0.144,  P[>=2 errors] ~ 1.0e-2  (1.0%)
//
// So at BER <= 1e-3 the (Correctable, DUE, Escape) proportions reported by
// this model match an exact per-bit Binomial decode to within ~1% relative
// error -- comfortably below the seed-to-seed Monte Carlo noise reported in
// pressure_points.csv. Above 1e-3 the *classification* is still correct
// (per-word draws handle multi-error words explicitly), but the marginal
// rates start to diverge from a real DRAM channel's because (a) the Poisson
// tail mass on >=3 errors per word becomes non-negligible and (b) the
// independent-bit assumption breaks down in practice (row hammer, multi-bit
// upset clustering). EccGuard emits a warning when a policy entry's BER
// exceeds this bound; raise it deliberately, with a corresponding note in
// the paper, if you sweep above it.
//
// Note on chipkill threshold approximation:
//   The "<=4 bits Correctable, <=8 bits DUE, else Escape" rule is optimistic
//   for uniformly-scattered bit errors (a true chipkill code corrects any
//   single x4-chip pattern, not any 4 bits). At BER <= 1e-3 the modal
//   chipkill-word event has 1 error in 1 chip, where the optimistic and
//   exact thresholds agree; the disagreement is again < the tail mass on
//   >=2 errors in a word.
// =====================================================================

constexpr double kEccBerTightUpperBound = 1.0e-3;

// Data bytes per ECC protection word, by scheme. 0 means "no word concept";
// the line is treated as a single bag of bits (used for EccScheme::NONE and
// as a fallback if the scheme is unknown).
inline uint32_t eccWordBytes(EccScheme scheme) {
    switch (scheme) {
    case EccScheme::NONE:        return 0;
    case EccScheme::SECDED_64:   return 8;   // 64-bit data per SECDED word
    case EccScheme::CHIPKILL_x4: return 16;  // 128-bit data per chipkill word
    }
    return 0;
}

// Classify a single ECC word given its bit-error count.
// For CHIPKILL_x4 this uses optimistic per-bit thresholds; prefer
// classifyEccWordChipAware when per-chip error counts are available.
inline EccOutcome classifyEccWord(unsigned word_errors, EccScheme scheme) {
    if (word_errors == 0) return EccOutcome::Clean;
    switch (scheme) {
    case EccScheme::NONE:
        return EccOutcome::SilentEscape;
    case EccScheme::SECDED_64:
        if (word_errors == 1) return EccOutcome::Correctable;
        if (word_errors == 2) return EccOutcome::DetectableUncorrectable;
        return EccOutcome::SilentEscape;
    case EccScheme::CHIPKILL_x4:
        if (word_errors <= 4) return EccOutcome::Correctable;
        if (word_errors <= 8) return EccOutcome::DetectableUncorrectable;
        return EccOutcome::SilentEscape;
    }
    return EccOutcome::SilentEscape;
}

// Number of x4 chips per ECC word. Only meaningful for CHIPKILL_x4.
inline unsigned chipsPerEccWord(EccScheme scheme) {
    if (scheme == EccScheme::CHIPKILL_x4)
        return eccWordBytes(scheme) * 8 / 4;  // 128 data bits / 4 bits per chip = 32
    return 0;
}

// Chip-aware classification for CHIPKILL_x4. chip_error_counts[i] is the
// number of bit errors landing in the i-th x4 chip of the word.
// A chipkill x4 code corrects any single-symbol (single-chip) error
// pattern, detects 2-symbol errors, and silently escapes on 3+.
// Falls back to classifyEccWord for non-chipkill schemes.
inline EccOutcome classifyEccWordChipAware(
        const std::vector<uint8_t>& chip_error_counts, EccScheme scheme) {
    if (scheme != EccScheme::CHIPKILL_x4)
        return classifyEccWord(
            [&]() -> unsigned {
                unsigned s = 0;
                for (auto c : chip_error_counts) s += c;
                return s;
            }(), scheme);
    unsigned affected = 0;
    for (auto c : chip_error_counts)
        if (c > 0) ++affected;
    if (affected == 0) return EccOutcome::Clean;
    if (affected == 1) return EccOutcome::Correctable;
    if (affected == 2) return EccOutcome::DetectableUncorrectable;
    return EccOutcome::SilentEscape;
}

// Result of aggregating per-word outcomes into a single line outcome.
struct EccLineOutcome {
    EccOutcome outcome     = EccOutcome::Clean;
    unsigned   escape_bits = 0;   // bits-on-the-wire from escaping words only
    unsigned   total_errors = 0;  // sum across all words (book-keeping)
};

// Aggregate per-word error counts into a single line outcome.
//   - Line outcome = worst per-word outcome.
//   - escape_bits  = sum of word_errors over words whose outcome is
//                    SilentEscape (these bits actually reach the consumer).
// Per-word errors in Correctable words contribute 0 leaked bits (ECC fixes
// them); DUE words are intercepted before delivery (LatencyOnly path keeps
// the bits in the wire but they are unreliable -- existing behaviour).
inline EccLineOutcome aggregateLineOutcome(const std::vector<unsigned>& per_word_errors,
                                           EccScheme scheme) {
    EccLineOutcome r;
    for (unsigned e : per_word_errors) {
        r.total_errors += e;
        EccOutcome o = classifyEccWord(e, scheme);
        if (static_cast<uint8_t>(o) > static_cast<uint8_t>(r.outcome)) r.outcome = o;
        if (o == EccOutcome::SilentEscape) r.escape_bits += e;
    }
    return r;
}

// Chip-aware variant for CHIPKILL_x4. Uses per-chip error distribution
// instead of per-bit thresholds for more accurate classification.
inline EccLineOutcome aggregateLineOutcomeChipAware(
        const std::vector<unsigned>& per_word_errors,
        const std::vector<std::vector<uint8_t>>& per_word_chip_errors,
        EccScheme scheme) {
    if (scheme != EccScheme::CHIPKILL_x4
        || per_word_chip_errors.size() != per_word_errors.size()) {
        return aggregateLineOutcome(per_word_errors, scheme);
    }
    EccLineOutcome r;
    for (size_t w = 0; w < per_word_errors.size(); ++w) {
        r.total_errors += per_word_errors[w];
        EccOutcome o = classifyEccWordChipAware(per_word_chip_errors[w], scheme);
        if (static_cast<uint8_t>(o) > static_cast<uint8_t>(r.outcome)) r.outcome = o;
        if (o == EccOutcome::SilentEscape) r.escape_bits += per_word_errors[w];
    }
    return r;
}

// Back-compat wrapper for code that still passes a single "total errors on
// the line" count. This is the OLD conservative single-word approximation:
// it treats every bit error as if it landed in one protection word, which
// dramatically over-counts DUE/Escape outcomes whenever the line carries
// errors in multiple words. New code paths should call classifyEccWord on
// per-word draws and then aggregateLineOutcome instead.
inline EccOutcome classifyEccOutcome(unsigned num_bit_errors,
                                     uint32_t /*payload_bytes*/,
                                     EccScheme scheme) {
    return classifyEccWord(num_bit_errors, scheme);
}

} // namespace Carcosa
} // namespace SST

#endif /* SST_ELEMENTS_CARCOSA_ECC_SCHEME_H */
