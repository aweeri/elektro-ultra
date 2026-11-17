// Front-end DSP: DC blocker, NCO, rational resampler, fractional delay, acquisition.
#include "dsp/resample.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <algorithm>
#include <numeric>

#include "core/fft.h"
#include "dsp/filters.h"

namespace eu {

// One-pole high-pass, corner (1-r)*fs/(2*pi): r=0.9995 at 3 MSPS is ~240 Hz, clears LO leakage without touching the 220 kHz LRIT band.
void DcBlocker::process(const cf* in, cf* out, size_t n) {
    if (!enabled) {
        if (out != in) std::copy(in, in + n, out);
        return;
    }
    const float a = (float)(1.0 - r_);
    const float b = (float)r_;
    cf z = z_;
    for (size_t i = 0; i < n; ++i) {
        const cf x = in[i];
        z = cf(b * z.real() + a * x.real(), b * z.imag() + a * x.imag());
        out[i] = x - z;
    }
    z_ = z;
}

void Nco::process(const cf* in, cf* out, size_t n) {
    // Recurrence, renormalised so the phasor cannot drift off the unit circle.
    const double c = std::cos(df_), s = std::sin(df_);
    cf w((float)c, (float)-s);            // step for exp(-j*theta)
    cf ph((float)std::cos(phase_), (float)-std::sin(phase_));
    for (size_t i = 0; i < n; ++i) {
        out[i] = in[i] * ph;
        ph = ph * w;
        if ((i & 0x3FF) == 0x3FF) {       // every 1024 samples, renormalise
            const float m = std::sqrt(ph.real() * ph.real() + ph.imag() * ph.imag());
            if (m > 0) ph = cf(ph.real() / m, ph.imag() / m);
        }
    }
    phase_ = wrap_pi(std::atan2(-ph.imag(), ph.real()));
}

// Zeroth-order modified Bessel, same definition as filters.cpp.
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

static long long gcd_ll(long long a, long long b) {
    while (b) { long long t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

bool Resampler::configure(double fs_in, double fs_out, double atten_db, int span) {
    if (fs_in <= 0 || fs_out <= 0) return false;
    long long a = (long long)std::llround(fs_in);
    long long b = (long long)std::llround(fs_out);
    if (a <= 0 || b <= 0) return false;
    const long long g = gcd_ll(a, b);
    L_ = (int)(b / g);
    M_ = (int)(a / g);
    if (L_ > 8192 || M_ > 8192) return false;
    if (span < 4) span = 4;

    // Cutoff at the lower Nyquist: what folds there is removed by the RRC.
    const int P = (int)((2.0 * span * std::max(L_, M_) + L_ - 1) / L_) + 1;
    P_ = P;
    const int len = L_ * P;
    h_.assign((size_t)len, 0.0f);

    const double fc = 0.5 / (double)std::max(L_, M_);   // cycles/sample, upsampled
    const double beta = kaiser_beta(atten_db);
    const double i0b = bessel_i0(beta);
    const double center = 0.5 * (len - 1);
    double sum = 0.0;
    for (int i = 0; i < len; ++i) {
        const double t = (double)i - center;
        const double sinc = (std::fabs(t) < 1e-12)
                                ? 1.0
                                : std::sin(kPi * 2.0 * fc * t) / (kPi * 2.0 * fc * t);
        const double u = 2.0 * (double)i / (double)(len - 1) - 1.0;
        const double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0b;
        const double v = 2.0 * fc * sinc * w;
        h_[(size_t)i] = (float)v;
        sum += v;
    }
    // Per-branch unity DC gain: normalising the whole prototype divides output by L and costs 40 dB.
    if (sum != 0.0) {
        const float k = (float)((double)L_ / sum);
        for (auto& v : h_) v *= k;
    }

    // Phase-major, tap-reversed so both sides of the dot product are contiguous; reading h_ directly is a stride-L_ gather.
    htab_.assign((size_t)L_ * (size_t)P, 0.0f);
    for (int ph = 0; ph < L_; ++ph)
        for (int p = 0; p < P; ++p)
            htab_[(size_t)ph * (size_t)P + (size_t)p] =
                h_[(size_t)(P - 1 - p) * (size_t)L_ + (size_t)ph];

    // Doubled taps so one FMA covers 4 complex samples.
    htab2_.assign((size_t)L_ * (size_t)P * 2, 0.0f);
    for (size_t k = 0; k < (size_t)L_ * (size_t)P; ++k) {
        htab2_[2 * k] = htab_[k];
        htab2_[2 * k + 1] = htab_[k];
    }
    buf_.clear();
    buf_start_ = 0;
    n_in_ = 0;
    k_out_ = 0;
    base_ = 0;
    phase_ = 0;
    return true;
}

void Resampler::reset() {
    buf_.clear();
    buf_start_ = 0;
    n_in_ = 0;
    k_out_ = 0;
    base_ = 0;
    phase_ = 0;
}

void Resampler::process(const cf* in, size_t n, std::vector<cf>& out) {
    if (L_ <= 0) {
        out.insert(out.end(), in, in + n);
        return;
    }
    buf_.insert(buf_.end(), in, in + n);
    n_in_ += (long long)n;

    const long long P = P_;
    auto advance = [&] {
        phase_ += M_;
        while (phase_ >= L_) {
            phase_ -= L_;
            ++base_;
        }
    };

    for (;;) {
        if (base_ >= n_in_) break;                 // need more input
        if (base_ - P + 1 < buf_start_) {          // not enough history yet
            advance();
            continue;
        }
        const cf* x = &buf_[(size_t)(base_ - P + 1 - buf_start_)];
        float re = 0.0f, im = 0.0f;
#if defined(__AVX2__)
        const float* xf = reinterpret_cast<const float*>(x);
        const float* cf2 = &htab2_[(size_t)phase_ * (size_t)P * 2];
        __m256 acc = _mm256_setzero_ps();
        long long p = 0;
        for (; p + 4 <= P; p += 4)
            acc = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 2 * p), _mm256_loadu_ps(cf2 + 2 * p), acc);
        __m128 s = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        re = _mm_cvtss_f32(s);
        im = _mm_cvtss_f32(_mm_shuffle_ps(s, s, 0x55));
        const float* c = &htab_[(size_t)phase_ * (size_t)P];
        for (; p < P; ++p) {
            re += x[p].real() * c[p];
            im += x[p].imag() * c[p];
        }
#else
        const float* c = &htab_[(size_t)phase_ * (size_t)P];
        float ar0 = 0.0f, ai0 = 0.0f, ar1 = 0.0f, ai1 = 0.0f;
        float ar2 = 0.0f, ai2 = 0.0f, ar3 = 0.0f, ai3 = 0.0f;
        long long p = 0;
        for (; p + 4 <= P; p += 4) {
            ar0 += x[p].real() * c[p];         ai0 += x[p].imag() * c[p];
            ar1 += x[p + 1].real() * c[p + 1]; ai1 += x[p + 1].imag() * c[p + 1];
            ar2 += x[p + 2].real() * c[p + 2]; ai2 += x[p + 2].imag() * c[p + 2];
            ar3 += x[p + 3].real() * c[p + 3]; ai3 += x[p + 3].imag() * c[p + 3];
        }
        for (; p < P; ++p) {
            ar0 += x[p].real() * c[p];
            ai0 += x[p].imag() * c[p];
        }
        re = (ar0 + ar1) + (ar2 + ar3);
        im = (ai0 + ai1) + (ai2 + ai3);
#endif
        out.push_back(cf(re, im));
        advance();
    }

    // Trim history: keep everything the next output still needs, plus margin.
    const long long keep_from = base_ - P + 1 - 16;
    if (keep_from - buf_start_ > 1 << 16) {
        const long long drop = keep_from - buf_start_;
        buf_.erase(buf_.begin(), buf_.begin() + (size_t)drop);
        buf_start_ = keep_from;
    }
}

// Running-power normaliser, per block, so one sqrt per block not per sample.
void Agc::configure(double tau_samples, double target_rms) {
    if (tau_samples < 1.0) tau_samples = 1.0;
    alpha_ = 1.0 / tau_samples;
    target_ = target_rms;
    p_ = 0.0;
    g_ = 1.0;
    primed_ = false;
}

void Agc::process(const cf* in, cf* out, size_t n) {
    if (n == 0) return;
    const double a = 1.0 - std::exp(-(double)n * alpha_);
    double p = 0.0;
    for (size_t i = 0; i < n; ++i) p += std::norm(in[i]);
    p /= (double)n;
    p_ += a * (p - p_);
    if (p_ < 1e-20) p_ = 1e-20;
    const double g = target_ / std::sqrt(p_);
    g_ += a * (g - g_);                       // smooth the gain itself
    const float gf = (float)g_;
    for (size_t i = 0; i < n; ++i) out[i] = in[i] * gf;
}

cf FracDelay::at(double t) const {
    if (t < 0) t = 0;
    const int i = (int)std::floor(t);
    const double u = t - (double)i;
    auto get = [&](int k) -> cf {
        if (k < 0) k = 0;
        if (k >= (int)n_) k = (int)n_ - 1;
        size_t idx = (pos_ + n_ - 1 - (size_t)k) % n_;
        return buf_[idx];
    };
    // p1 is the sample at delay i and p2 at i+1; swapped, the fractional part runs the wrong way and no timing loop can correct it.
    const cf xm1 = get(i - 1), x0 = get(i), x1 = get(i + 1), x2 = get(i + 2);
    const cf a = x0 * 2.0f;
    const cf b = (x1 - xm1);
    const cf c = xm1 * 2.0f - x0 * 5.0f + x1 * 4.0f - x2;
    const cf d = -xm1 + x0 * 3.0f - x1 * 3.0f + x2;
    const float uf = (float)u;
    return 0.5f * (a + b * uf + c * uf * uf + d * uf * uf * uf);
}

double band_centroid(const cf* x, size_t n, double fs, double search_hz, double* width_hz) {
    if (n < 4096) return 0.0;
    const size_t N = 4096;
    const size_t segs = std::min<size_t>(8, n / N);
    Fft fft(N);
    std::vector<double> acc(N, 0.0);
    std::vector<cf> tmp(N);
    auto w = hann(N);
    for (size_t s = 0; s < segs; ++s) {
        for (size_t i = 0; i < N; ++i) tmp[i] = x[s * N + i] * w[i];
        fft.forward(tmp.data());
        for (size_t i = 0; i < N; ++i) acc[i] += std::norm(tmp[i]);
    }
    fft.fftshift(acc.data());
    double num = 0.0, den = 0.0;
    double lo = 0.0, hi = 0.0;
    bool have = false;
    const double df = fs / (double)N;
    for (size_t i = 1; i + 1 < N; ++i) {
        const double f = ((double)i - (double)N * 0.5) * df;
        if (std::fabs(f) > search_hz) continue;
        const double p = acc[i];
        num += f * p;
        den += p;
        if (!have) { lo = hi = f; have = true; }
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    if (width_hz) *width_hz = have ? (hi - lo) : 0.0;
    return den > 0 ? num / den : 0.0;
}

double acquire_carrier(const cf* x, size_t n, double fs, double offset_guess, int order,
                       double search_hz, double& strength_db) {
    strength_db = -200.0;
    if (n < 8192 || order < 2) return offset_guess;
    size_t N = 1;
    while (N * 2 <= n && N < (1u << 20)) N *= 2;
    if (N < 8192) return offset_guess;

    // x^2 (BPSK) / x^4 (QPSK) turns the offset into a pure tone; measuring it beats the top bin of a wide FFT.
    std::vector<cf> y(N);
    for (size_t i = 0; i < N; ++i) {
        cf v = x[i];
        for (int k = 1; k < order; ++k) v = v * x[i];
        y[i] = v;
    }
    auto w = hann(N);
    for (size_t i = 0; i < N; ++i) y[i] *= w[i];
    Fft fft(N);
    fft.forward(y.data());
    std::vector<double> p(N);
    for (size_t i = 0; i < N; ++i) p[i] = std::norm(y[i]);
    fft.fftshift(p.data());

    double f_exp = order * offset_guess;
    while (f_exp >= fs * 0.5) f_exp -= fs;
    while (f_exp < -fs * 0.5) f_exp += fs;

    const double df = fs / (double)N;
    long long lo = (long long)std::floor((f_exp - search_hz) / df) + (long long)(N / 2);
    long long hi = (long long)std::ceil((f_exp + search_hz) / df) + (long long)(N / 2);
    lo = std::max<long long>(lo, 1);
    hi = std::min<long long>(hi, (long long)N - 2);
    if (hi <= lo) return offset_guess;

    size_t best = (size_t)lo;
    double bv = -1.0;
    for (long long i = lo; i <= hi; ++i) {
        if (p[(size_t)i] > bv) { bv = p[(size_t)i]; best = (size_t)i; }
    }
    std::vector<double> loc;
    loc.reserve((size_t)(hi - lo + 1));
    for (long long i = lo; i <= hi; ++i) loc.push_back(p[(size_t)i]);
    std::nth_element(loc.begin(), loc.begin() + loc.size() / 2, loc.end());
    const double med = loc[loc.size() / 2];
    strength_db = med > 0 ? 10.0 * std::log10(bv / med) : 0.0;

    const double frac = parabolic_offset(p.data(), N, best);
    const double f_peak = ((double)best - (double)N * 0.5 + frac) * df;
    return f_peak / (double)order;
}

}  // namespace eu
