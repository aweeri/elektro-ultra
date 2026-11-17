#include "dsp/filters.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <cmath>

namespace eu {

double kaiser_beta(double atten_db) {
    if (atten_db <= 21.0) return 0.0;
    if (atten_db <= 50.0) return 0.5842 * std::pow(atten_db - 21.0, 0.4) + 0.07886 * (atten_db - 21.0);
    return 0.1102 * (atten_db - 8.7);
}

// Zeroth-order modified Bessel, series form.
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

std::vector<float> design_lowpass(double cutoff, int ntaps, double beta) {
    if (ntaps < 3) ntaps = 3;
    if ((ntaps & 1) == 0) ++ntaps;
    std::vector<float> h(ntaps);
    const int mid = ntaps / 2;
    const double i0b = bessel_i0(beta);
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        const double n = (double)(i - mid);
        const double sinc = (n == 0.0) ? 2.0 * cutoff
                                       : std::sin(2.0 * kPi * cutoff * n) / (kPi * n);
        const double r = n / (double)mid;
        const double w = (std::fabs(r) >= 1.0) ? 0.0
                                              : bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0b;
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    if (sum != 0.0) for (auto& v : h) v = (float)(v / sum);
    return h;
}

std::vector<float> design_rrc(double alpha, int sps, int span) {
    if (sps < 1) sps = 1;
    if (span < 1) span = 1;
    const int ntaps = span * sps + 1;
    std::vector<float> h(ntaps);
    const double mid = (double)(ntaps - 1) / 2.0;
    const double Ts = (double)sps;
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        const double t = ((double)i - mid) / Ts;   // symbol periods
        double v;
        const double eps = 1e-9;
        if (std::fabs(t) < eps) {
            v = 1.0 - alpha + 4.0 * alpha / kPi;
        } else if (alpha > 0 && std::fabs(std::fabs(t) - 1.0 / (4.0 * alpha)) < eps) {
            const double s = std::sin(kPi / (4.0 * alpha));
            const double c = std::cos(kPi / (4.0 * alpha));
            v = alpha / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * s + (1.0 - 2.0 / kPi) * c);
        } else {
            const double num = std::sin(kPi * t * (1.0 - alpha)) +
                               4.0 * alpha * t * std::cos(kPi * t * (1.0 + alpha));
            const double den = kPi * t * (1.0 - std::pow(4.0 * alpha * t, 2.0));
            v = num / den;
        }
        h[i] = (float)v;
        sum += v;
    }
    if (sum != 0.0) for (auto& v : h) v = (float)(v / sum);
    return h;
}

void Fir::set_taps(std::vector<float> taps) {
    taps_ = std::move(taps);
    reset();
}

void Fir::reset() {
    hist_.assign(taps_.size() > 0 ? taps_.size() - 1 : 0, cf(0, 0));
    pos_ = 0;
    hb_.clear();
}

// Contiguous history, not a ring: the rotating index made the RRC 39 % of a decode by defeating vectorisation.
void Fir::process(const cf* in, cf* out, size_t n) {
    const size_t nt = taps_.size();
    if (nt == 0) {
        for (size_t i = 0; i < n; ++i) out[i] = in[i];
        return;
    }
    if (trev_.size() != nt) {
        trev_.assign(taps_.rbegin(), taps_.rend());
        // Tap stored twice so a complex sample times a real tap is one FMA.
        taps2_.resize(nt * 2);
        for (size_t j = 0; j < nt; ++j) {
            taps2_[2 * j] = taps_[nt - 1 - j];
            taps2_[2 * j + 1] = taps_[nt - 1 - j];
        }
    }
    if (hist_.size() != nt - 1) reset();

    const size_t hist = nt - 1;
    hb_.resize(hist + n);
    std::memcpy(hb_.data(), hist_.data(), hist * sizeof(cf));
    std::memcpy(hb_.data() + hist, in, n * sizeof(cf));

    const cf* t = trev_.data();
    for (size_t i = 0; i < n; ++i) {
        const cf* w = hb_.data() + i;
#if defined(__AVX2__)
        const float* wf = reinterpret_cast<const float*>(w);
        const float* tf = taps2_.data();
        __m256 acc = _mm256_setzero_ps();
        size_t j = 0;
        for (; j + 4 <= nt; j += 4)
            acc = _mm256_fmadd_ps(_mm256_loadu_ps(wf + 2 * j), _mm256_loadu_ps(tf + 2 * j), acc);
        // Lanes [r0,i0,r1,i1,...]: fold to one complex value.
        __m128 s = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        float re = _mm_cvtss_f32(s);
        float im = _mm_cvtss_f32(_mm_shuffle_ps(s, s, 0x55));
        for (; j < nt; ++j) {
            re += w[j].real() * t[j].real();
            im += w[j].imag() * t[j].real();
        }
        out[i] = cf(re, im);
#else
        float ar0 = 0.0f, ai0 = 0.0f, ar1 = 0.0f, ai1 = 0.0f;
        float ar2 = 0.0f, ai2 = 0.0f, ar3 = 0.0f, ai3 = 0.0f;
        size_t j = 0;
        for (; j + 4 <= nt; j += 4) {
            ar0 += w[j].real() * t[j].real();         ai0 += w[j].imag() * t[j].real();
            ar1 += w[j + 1].real() * t[j + 1].real(); ai1 += w[j + 1].imag() * t[j + 1].real();
            ar2 += w[j + 2].real() * t[j + 2].real(); ai2 += w[j + 2].imag() * t[j + 2].real();
            ar3 += w[j + 3].real() * t[j + 3].real(); ai3 += w[j + 3].imag() * t[j + 3].real();
        }
        for (; j < nt; ++j) {
            ar0 += w[j].real() * t[j].real();
            ai0 += w[j].imag() * t[j].real();
        }
        out[i] = cf((ar0 + ar1) + (ar2 + ar3), (ai0 + ai1) + (ai2 + ai3));
#endif
    }

    std::memcpy(hist_.data(), hb_.data() + n, hist * sizeof(cf));
}

void convolve_same(const std::vector<cf>& in, const std::vector<float>& taps,
                   std::vector<cf>& out) {
    const size_t n = in.size();
    out.assign(n, cf(0, 0));
    if (taps.empty() || n == 0) return;
    const size_t nt = taps.size();
    const size_t mid = nt / 2;
    for (size_t i = 0; i < n; ++i) {
        cf acc(0, 0);
        for (size_t j = 0; j < nt; ++j) {
            const long idx = (long)i + (long)j - (long)mid;
            if (idx < 0 || idx >= (long)n) continue;
            acc += in[(size_t)idx] * taps[j];
        }
        out[i] = acc;
    }
}

}  // namespace eu
