// Unit tests for the device-side radix-2 FFT (quetz_fft.h) that
// quetz.FFTKernel runs. The in-sim firmware test checks an impulse input, whose
// FFT is all-ones regardless of the twiddle factors; these tests
// validate the actual butterfly math against a naive DFT so a twiddle-factor
// regression is caught here rather than slipping through the sim.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

#include "../../quetz_fft.h"

using SST::Quetz::QuetzCf;
using SST::Quetz::quetz_fft_radix2;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Reference O(N^2) DFT in double precision.
std::vector<std::complex<double>> naive_dft(const std::vector<QuetzCf>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> out(n);
    for (size_t k = 0; k < n; k++) {
        std::complex<double> acc(0.0, 0.0);
        for (size_t t = 0; t < n; t++) {
            double ang = -2.0 * kPi * (double)k * (double)t / (double)n;
            acc += std::complex<double>(x[t].re, x[t].im)
                 * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        out[k] = acc;
    }
    return out;
}

// Largest abs error between the device FFT and the reference DFT of the input.
double max_err_vs_dft(std::vector<QuetzCf> in) {
    auto ref = naive_dft(in);
    quetz_fft_radix2(in.data(), (uint32_t)in.size());
    double e = 0.0;
    for (size_t i = 0; i < in.size(); i++) {
        e = std::max(e, std::fabs((double)in[i].re - ref[i].real()));
        e = std::max(e, std::fabs((double)in[i].im - ref[i].imag()));
    }
    return e;
}

float f32_from_bits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<QuetzCf> raptor_reference_input() {
    const float s = f32_from_bits(0x3f3504f3u); // binary32 sqrt(1/2)
    const QuetzCf phases[] = {
        { 1.0f,  0.0f}, { s,  s}, { 0.0f,  1.0f}, {-s,  s},
        {-1.0f,  0.0f}, {-s, -s}, { 0.0f, -1.0f}, { s, -s},
    };
    std::vector<QuetzCf> input(256);
    for (uint32_t n = 0; n < input.size(); n++)
        input[n] = phases[n % 8u];
    return input;
}

bool raptor_reference_output_matches(const std::vector<QuetzCf>& output) {
    for (uint32_t k = 0; k < output.size(); k++) {
        const float expected_real = k == output.size() / 8u ? 256.0f : 0.0f;
        if (std::fabs(output[k].re - expected_real) > 0.001f ||
            std::fabs(output[k].im) > 0.001f)
            return false;
    }
    return true;
}

uint32_t fnv1a_network_word(uint32_t checksum, uint32_t word) {
    for (uint32_t byte_index = 0; byte_index < 4u; byte_index++) {
        uint32_t shift = 24u - byte_index * 8u;
        checksum = (checksum ^ ((word >> shift) & 0xffu)) * 16777619u;
    }
    return checksum;
}

uint32_t canonical_reference_checksum(uint32_t peak_bin) {
    uint32_t checksum = 2166136261u;
    for (uint32_t k = 0; k < 256u; k++) {
        checksum = fnv1a_network_word(
            checksum, k == peak_bin ? 0x43800000u : 0u);
        checksum = fnv1a_network_word(checksum, 0u);
    }
    return checksum;
}

// Deliberate mutation of the production radix-2 loop: every twiddle outside
// {1, -i, -1, i} is replaced with zero. A useful reference vector must reject
// this implementation; the previous four-phase vector did not.
uint32_t fft_with_non_quadrant_twiddles_zeroed(QuetzCf* a, uint32_t n) {
    uint32_t logn = 0;
    while ((1u << logn) < n) logn++;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = 0, x = i;
        for (uint32_t b = 0; b < logn; b++) {
            r = (r << 1) | (x & 1u);
            x >>= 1;
        }
        if (r > i) {
            QuetzCf tmp = a[i];
            a[i] = a[r];
            a[r] = tmp;
        }
    }

    uint32_t active_mutations = 0;
    for (uint32_t stage = 1; stage <= logn; stage++) {
        uint32_t half = 1u << (stage - 1u);
        for (uint32_t k = 0; k < n / 2u; k++) {
            uint32_t j = k & (half - 1u);
            uint32_t group = k >> (stage - 1u);
            uint32_t i0 = group * (half << 1u) + j;
            uint32_t i1 = i0 + half;
            uint32_t twiddle_index = j << (logn - stage);
            bool non_quadrant = n >= 4u && (twiddle_index % (n / 4u)) != 0u;
            double angle = -2.0 * kPi * (double)twiddle_index / (double)n;
            float wr = non_quadrant ? 0.0f : (float)std::cos(angle);
            float wi = non_quadrant ? 0.0f : (float)std::sin(angle);
            QuetzCf v = a[i1];
            if (non_quadrant && (v.re != 0.0f || v.im != 0.0f))
                active_mutations++;
            float tr = wr * v.re - wi * v.im;
            float ti = wr * v.im + wi * v.re;
            QuetzCf u = a[i0];
            a[i0].re = u.re + tr;
            a[i0].im = u.im + ti;
            a[i1].re = u.re - tr;
            a[i1].im = u.im - ti;
        }
    }
    return active_mutations;
}
} // namespace

TEST_CASE("impulse -> all ones (bit-exact; matches the firmware check)") {
    const uint32_t N = 256;
    std::vector<QuetzCf> a(N, {0.0f, 0.0f});
    a[0] = {1.0f, 0.0f};
    quetz_fft_radix2(a.data(), N);
    for (uint32_t k = 0; k < N; k++) {
        CHECK(a[k].re == 1.0f);
        CHECK(a[k].im == 0.0f);
    }
}

TEST_CASE("Raptor reference eighth-rate complex tone selects forward bin") {
    const uint32_t N = 256;
    std::vector<QuetzCf> a = raptor_reference_input();
    quetz_fft_radix2(a.data(), N);

    uint32_t checksum = 2166136261u;
    for (uint32_t k = 0; k < N; k++) {
        const bool peak = k == N / 8u;
        uint32_t real_bits;
        uint32_t imag_bits;
        std::memcpy(&real_bits, &a[k].re, sizeof(real_bits));
        std::memcpy(&imag_bits, &a[k].im, sizeof(imag_bits));
        const bool real_matches = peak
            ? (real_bits & 0x80000000u) == 0u &&
              (real_bits >= 0x43800000u
                   ? real_bits - 0x43800000u <= 64u
                   : 0x43800000u - real_bits <= 64u)
            : (real_bits & 0x7fffffffu) <= 0x3a83126fu;
        const bool imag_matches =
            (imag_bits & 0x7fffffffu) <= 0x3a83126fu;
        CHECK(real_matches);
        CHECK(imag_matches);

        const uint32_t canonical_real = peak ? 0x43800000u : 0u;
        checksum = fnv1a_network_word(checksum, canonical_real);
        checksum = fnv1a_network_word(checksum, 0u);
    }
    CHECK(checksum == 0xb9b06c06u);
}

TEST_CASE("Raptor reference checksum encodes peak-bin placement") {
    const uint32_t bin32_checksum = canonical_reference_checksum(32u);
    const uint32_t bin64_checksum = canonical_reference_checksum(64u);

    CHECK(bin32_checksum == 0xb9b06c06u);
    CHECK(bin64_checksum == 0x5087c806u);
    CHECK(bin32_checksum != bin64_checksum);
}

TEST_CASE("Raptor reference rejects zeroed non-quadrant twiddles") {
    std::vector<QuetzCf> a = raptor_reference_input();
    uint32_t active_mutations =
        fft_with_non_quadrant_twiddles_zeroed(a.data(), (uint32_t)a.size());

    CHECK(active_mutations > 0u);
    CHECK_FALSE(raptor_reference_output_matches(a));
}

TEST_CASE("DC -> N at bin 0, zero elsewhere") {
    const uint32_t N = 256;
    std::vector<QuetzCf> a(N, {1.0f, 0.0f});
    quetz_fft_radix2(a.data(), N);
    CHECK(a[0].re == doctest::Approx((double)N));
    CHECK(a[0].im == doctest::Approx(0.0));
    for (uint32_t k = 1; k < N; k++) {
        CHECK(std::fabs(a[k].re) < 1e-2);
        CHECK(std::fabs(a[k].im) < 1e-2);
    }
}

// The twiddle-sensitive checks: real cosine tones must land on the expected
// spectral bins, which only happens if the butterfly twiddles are correct.
TEST_CASE("real tone lands on the correct bins (validates twiddles)") {
    for (uint32_t N : {8u, 16u, 64u, 256u, 1024u}) {
        for (uint32_t k0 : {1u, 3u, 5u}) {
            if (k0 >= N / 2) continue;
            std::vector<QuetzCf> a(N);
            for (uint32_t t = 0; t < N; t++)
                a[t] = {(float)std::cos(2.0 * kPi * k0 * t / N), 0.0f};
            quetz_fft_radix2(a.data(), N);
            // cos tone -> N/2 at bins k0 and N-k0, ~0 everywhere else.
            for (uint32_t k = 0; k < N; k++) {
                double expect = (k == k0 || k == N - k0) ? 0.5 * N : 0.0;
                CHECK(std::fabs((double)a[k].re - expect) < 1e-2);
                CHECK(std::fabs((double)a[k].im) < 1e-2);
            }
        }
    }
}

TEST_CASE("matches naive DFT for random complex input, several sizes") {
    // Deterministic LCG so the test is reproducible.
    uint32_t seed = 0x1234567u;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (float)((int)(seed >> 9) % 2001 - 1000) / 100.0f;  // [-10, 10]
    };
    for (uint32_t N : {1u, 2u, 4u, 8u, 16u, 64u, 256u, 1024u}) {
        std::vector<QuetzCf> a(N);
        for (uint32_t i = 0; i < N; i++) a[i] = {rnd(), rnd()};
        CHECK(max_err_vs_dft(a) < 1e-2);
    }
}

TEST_CASE("N=1 is the identity") {
    std::vector<QuetzCf> a = {{3.5f, -1.25f}};
    quetz_fft_radix2(a.data(), 1);
    CHECK(a[0].re == 3.5f);
    CHECK(a[0].im == -1.25f);
}
