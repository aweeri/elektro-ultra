// Front-end DSP: DC blocker, NCO, rational resampler, fractional delay, acquisition.
#include "dsp/resample.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <algorithm>
#include <numeric>

#include "core/parallel.h"

#include "core/fft.h"
#include "dsp/filters.h"

namespace eu {

// One-pole high-pass, corner (1-r)*fs/(2*pi): r=0.9995 at 3 MSPS is ~240 Hz, clears LO leakage without touching the 220 kHz LRIT band.
void DcBlocker::process(const cf* in, cf* out, size_t n) {
    if (!enabled) {
        if (out != in) std::copy(in, in + n, out);
        return;
    }
    // One-pole IIR: the recurrence is serial, and splitting it costs an extra sweep
    // of the block that outweighed the gain.
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
    // Renormalised so the phasor cannot drift off the unit circle.
    const double c = std::cos(df_), s = std::sin(df_);
    const cf w((float)c, (float)-s);
    constexpr size_t kMin = 16384;
    const int nch = n >= kMin ? (int)std::min<size_t>(thread_count(), (n + kMin - 1) / kMin) : 1;
    auto chunk = [&](size_t i0, size_t i1) {
        // Phase is analytic in the sample index, so a chunk starts exactly, no warm-up.
        double ph = phase_ + df_ * (double)i0;
        ph -= 2.0 * kPi * std::floor(ph / (2.0 * kPi) + 0.5);
        cf p((float)std::cos(ph), (float)-std::sin(ph));
        for (size_t i = i0; i < i1; ++i) {
            out[i] = in[i] * p;
            p = p * w;
            if ((i & 0x3FF) == 0x3FF) {
                const float m = std::sqrt(p.real() * p.real() + p.imag() * p.imag());
                if (m > 0) p = cf(p.real() / m, p.imag() / m);
            }
        }
    };
    if (nch <= 1) {
        chunk(0, n);
    } else {
        const size_t step = (n + (size_t)nch - 1) / (size_t)nch;
        parallel_for(nch, [&](int ci) {
            const size_t i0 = (size_t)ci * step;
            const size_t i1 = std::min(n, i0 + step);
            if (i0 < i1) chunk(i0, i1);
        });
    }
    phase_ = phase_ + df_ * (double)n;
    phase_ -= 2.0 * kPi * std::floor(phase_ / (2.0 * kPi) + 0.5);
}

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

    // Cutoff at the lower Nyquist; the RRC removes what folds there.
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

    // Doubled taps: one FMA covers 4 complex samples.
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

    // Output t sits at base0 + floor((ph0 + t*M)/L) with phase (ph0 + t*M) % L. Closing
    // that form means the whole output range is addressable, so it splits across threads.
    const long long P = P_;
    const long long base0 = base_;
    const long long ph0 = phase_;
    const long long R = n_in_ - 1 - base0;
    long long t_max = -1;
    if (R >= 0) {
        const long long hi = (R + 1) * (long long)L_ - ph0;
        if (hi > 0) t_max = (hi - 1) / (long long)M_;
    }
    const long long S = buf_start_ + P - 1;
    long long t_min = 0;
    if (base0 < S) {
        const long long need = (S - base0) * (long long)L_ - ph0;
        t_min = need > 0 ? (need + (long long)M_ - 1) / (long long)M_ : 0;
    }

    if (t_max >= 0) {
        const long long done = t_max + 1;
        if (t_max >= t_min) {
            const size_t have = out.size();
            const long long count = t_max - t_min + 1;
            out.resize(have + (size_t)count);
            cf* dst = out.data() + have;

            const long long a0 = ph0 + t_min * (long long)M_;
            cf* const buf = buf_.data();
            const long long bs = buf_start_;
            const int L = L_, M = M_;
            auto chunk = [&](long long t0, long long t1) {
                long long num = ph0 + t0 * (long long)M_;
                long long base = base0 + num / L;
                int ph = (int)(num % L);
                for (long long t = t0; t < t1; ++t) {
                    const cf* x = buf + (base - P + 1 - bs);
                    float re = 0.0f, im = 0.0f;
#if defined(__AVX2__)
                    const float* xf = reinterpret_cast<const float*>(x);
                    const float* c2 = &htab2_[(size_t)ph * (size_t)P * 2];
                    __m256 acc = _mm256_setzero_ps();
                    long long k = 0;
                    for (; k + 4 <= P; k += 4)
                        acc = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 2 * k), _mm256_loadu_ps(c2 + 2 * k), acc);
                    __m128 sv = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
                    sv = _mm_add_ps(sv, _mm_movehl_ps(sv, sv));
                    re = _mm_cvtss_f32(sv);
                    im = _mm_cvtss_f32(_mm_shuffle_ps(sv, sv, 0x55));
                    const float* c1 = &htab_[(size_t)ph * (size_t)P];
                    for (; k < P; ++k) {
                        re += x[k].real() * c1[k];
                        im += x[k].imag() * c1[k];
                    }
#else
                    const float* c1 = &htab_[(size_t)ph * (size_t)P];
                    float ar0 = 0.0f, ai0 = 0.0f, ar1 = 0.0f, ai1 = 0.0f;
                    float ar2 = 0.0f, ai2 = 0.0f, ar3 = 0.0f, ai3 = 0.0f;
                    long long k = 0;
                    for (; k + 4 <= P; k += 4) {
                        ar0 += x[k].real() * c1[k];         ai0 += x[k].imag() * c1[k];
                        ar1 += x[k + 1].real() * c1[k + 1]; ai1 += x[k + 1].imag() * c1[k + 1];
                        ar2 += x[k + 2].real() * c1[k + 2]; ai2 += x[k + 2].imag() * c1[k + 2];
                        ar3 += x[k + 3].real() * c1[k + 3]; ai3 += x[k + 3].imag() * c1[k + 3];
                    }
                    for (; k < P; ++k) {
                        ar0 += x[k].real() * c1[k];
                        ai0 += x[k].imag() * c1[k];
                    }
                    re = (ar0 + ar1) + (ar2 + ar3);
                    im = (ai0 + ai1) + (ai2 + ai3);
#endif
                    dst[t - t_min] = cf(re, im);
                    ph += M;
                    while (ph >= L) {
                        ph -= L;
                        ++base;
                    }
                }
            };

            constexpr long long kMin = 8192;
            const int nch = count >= kMin
                                ? (int)std::min<size_t>(thread_count(),
                                                        (size_t)((count + kMin - 1) / kMin))
                                : 1;
            if (nch <= 1) {
                chunk(t_min, t_max + 1);
            } else {
                const long long step = (count + nch - 1) / nch;
                parallel_for(nch, [&](int c) {
                    const long long t0 = t_min + (long long)c * step;
                    const long long t1 = std::min(t_max + 1, t0 + step);
                    if (t0 < t1) chunk(t0, t1);
                });
            }
            (void)a0;
        }
        const long long num = ph0 + done * (long long)M_;
        base_ = base0 + num / (long long)L_;
        phase_ = (int)(num % (long long)L_);
        k_out_ += done;
    }

    const long long keep_from = base_ - P + 1 - 16;
    if (keep_from - buf_start_ > 1 << 16) {
        const long long drop = keep_from - buf_start_;
        buf_.erase(buf_.begin(), buf_.begin() + (size_t)drop);
        buf_start_ = keep_from;
    }
}

// Per-block power normaliser: one sqrt per block, not per sample.
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
