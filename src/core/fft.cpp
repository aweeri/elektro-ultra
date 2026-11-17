#include "core/fft.h"

#include <cmath>
#include <cstring>

namespace eu {

Fft::Fft(size_t n) : n_(n) {
    rev_.resize(n_);
    size_t bits = 0;
    while ((1u << bits) < n_) ++bits;
    for (size_t i = 0; i < n_; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
        rev_[i] = r;
    }
    tw_.resize(n_ / 2);
    for (size_t i = 0; i < n_ / 2; ++i)
        tw_[i] = cf((float)std::cos(-2.0 * kPi * (double)i / (double)n_),
                    (float)std::sin(-2.0 * kPi * (double)i / (double)n_));
}

void Fft::transform(cf* x, bool inverse) const {
    const size_t n = n_;
    for (size_t i = 0; i < n; ++i) {
        size_t j = rev_[i];
        if (j > i) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; ++k) {
                cf w = tw_[k * step];
                if (inverse) w = std::conj(w);
                cf u = x[i + k];
                cf v = x[i + k + half] * w;
                x[i + k] = u + v;
                x[i + k + half] = u - v;
            }
        }
    }
    if (inverse) {
        const float s = 1.0f / (float)n;
        for (size_t i = 0; i < n; ++i) x[i] *= s;
    }
}

void Fft::forward(cf* x) const { transform(x, false); }
void Fft::inverse(cf* x) const { transform(x, true); }

void Fft::power_spectrum(const cf* in, size_t nin, std::vector<double>& out) const {
    std::vector<cf> buf(n_, cf(0, 0));
    const size_t m = std::min(nin, n_);
    std::memcpy(buf.data(), in, m * sizeof(cf));
    transform(buf.data(), false);
    out.resize(n_);
    for (size_t i = 0; i < n_; ++i) out[i] = (double)std::norm(buf[i]);
}

void Fft::fftshift(cf* x) const {
    const size_t h = n_ / 2;
    for (size_t i = 0; i < h; ++i) std::swap(x[i], x[i + h]);
}
void Fft::fftshift(double* x) const {
    const size_t h = n_ / 2;
    for (size_t i = 0; i < h; ++i) std::swap(x[i], x[i + h]);
}

std::vector<float> hann(size_t n) {
    std::vector<float> w(n);
    if (n < 2) { if (n) w[0] = 1.0f; return w; }
    for (size_t i = 0; i < n; ++i)
        w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(n - 1)));
    return w;
}

double parabolic_offset(const double* p, size_t n, size_t idx) {
    if (idx == 0 || idx + 1 >= n) return 0.0;
    const double a = p[idx - 1], b = p[idx], c = p[idx + 1];
    const double den = (a - 2.0 * b + c);
    if (std::fabs(den) < 1e-30) return 0.0;
    return 0.5 * (a - c) / den;
}

}  // namespace eu
