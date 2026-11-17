// Streaming DSP primitives and the coarse frequency acquisition that feeds PLL.
#pragma once

#include <vector>

#include "core/common.h"

namespace eu {

class DcBlocker {
public:
    explicit DcBlocker(double r = 0.9995) : r_(r) {}
    void process(const cf* in, cf* out, size_t n);
    void reset() { z_ = cf(0, 0); }
    bool enabled = true;

private:
    double r_;
    cf z_{0, 0};
};

class Nco {
public:
    void set_freq_hz(double f, double fs) { df_ = 2.0 * M_PI * f / fs; }
    void set_phase(double p) { phase_ = p; }
    double freq_hz(double fs) const { return df_ * fs / (2.0 * M_PI); }
    // out = in * exp(-j*theta); out may alias in.
    void process(const cf* in, cf* out, size_t n);
    void reset() { phase_ = 0.0; }

private:
    double df_ = 0.0;
    double phase_ = 0.0;
};

// L/M polyphase resampler, output rate exactly fs_in*L/M.
class Resampler {
public:
    // atten_db: stopband target; span: zero crossings each side.
    bool configure(double fs_in, double fs_out, double atten_db = 80.0, int span = 12);
    void process(const cf* in, size_t n, std::vector<cf>& out);
    void reset();
    bool active() const { return L_ > 0; }
    double ratio() const { return L_ > 0 ? (double)L_ / (double)M_ : 1.0; }
    size_t taps_per_phase() const { return P_; }

private:
    int L_ = 0, M_ = 0, P_ = 0;
    std::vector<float> h_;      // prototype, p*L_ + phase
    std::vector<float> htab_;   // phase-major, reversed, for the inner loop
    std::vector<float> htab2_;  // each tap doubled, for the AVX2 path
    std::vector<cf> buf_;
    long long buf_start_ = 0;   // absolute index of buf_[0]
    long long n_in_ = 0;
    long long k_out_ = 0;
    // Incremental output position: recomputing base/phase from k_out_ costs two 64-bit divisions per sample, more than the filter.
    long long base_ = 0;        // absolute integer input index of the next output
    int phase_ = 0;             // polyphase branch of the next output
};

// Running-RMS AGC: keeps decision thresholds meaningful on captures whose level varies 40 dB between recordings (rms -29 to -46 dBFS).
class Agc {
public:
    // tau_samples: power-averaging time constant in samples.
    void configure(double tau_samples, double target_rms = 1.0);
    void process(const cf* in, cf* out, size_t n);
    void reset() { p_ = 0.0; primed_ = false; }
    double gain() const { return g_; }
    bool enabled = true;

private:
    double alpha_ = 1e-4, target_ = 1.0, p_ = 0.0, g_ = 1.0;
    bool primed_ = false;
};

// Cubic (Catmull-Rom) fractional delay over a ring of complex samples.
class FracDelay {
public:
    void reset(size_t n = 64) { buf_.assign(n, cf(0, 0)); pos_ = 0; n_ = n; }
    void push(cf v) {
        buf_[pos_] = v;
        pos_ = (pos_ + 1) % n_;
    }
    // Value at t samples before the newest sample; t in [0, n_-3).
    cf at(double t) const;

private:
    std::vector<cf> buf_;
    size_t pos_ = 0, n_ = 64;
};

// Coarse carrier estimate: offset_guess is the band centroid (Hz); returns refined offset and strength_db over the local median, using x^2/x^4 to strip modulation.
double acquire_carrier(const cf* x, size_t n, double fs, double offset_guess,
                       int order, double search_hz, double& strength_db);

// Band centroid from the averaged PSD, restricted to +-search_hz.
double band_centroid(const cf* x, size_t n, double fs, double search_hz,
                     double* width_hz = nullptr);

}  // namespace eu
