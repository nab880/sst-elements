// Unit tests for the device-side radix-2 FFT (quetz_fft.h) that QuetzGpuDevice
// runs in kernel_type=fft mode. The in-sim firmware test only checks an impulse
// input, whose FFT is all-ones regardless of the twiddle factors; these tests
// validate the actual butterfly math against a naive DFT so a twiddle-factor
// regression is caught here rather than slipping through the sim.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>
#include <complex>
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
