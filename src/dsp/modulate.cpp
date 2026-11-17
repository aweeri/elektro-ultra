#include "dsp/modulate.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "dsp/filters.h"

namespace eu {

int bits_per_symbol(const std::string& mod) {
    if (mod == "bpsk") return 1;
    if (mod == "qpsk" || mod == "oqpsk") return 2;
    return 1;
}

int symbols_per_frame_bits(const std::string& mod) { return bits_per_symbol(mod); }

std::vector<uint8_t> random_bits(size_t nsym, int bps, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 1);
    std::vector<uint8_t> b(nsym * (size_t)bps);
    for (auto& v : b) v = (uint8_t)d(rng);
    return b;
}

static inline double cubic(const double* y, double f) {
    const double c0 = y[1];
    const double c1 = 0.5 * (y[2] - y[0]);
    const double c2 = y[0] - 2.5 * y[1] + 2.0 * y[2] - 0.5 * y[3];
    const double c3 = 0.5 * (y[3] - y[0]) + 1.5 * (y[1] - y[2]);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

size_t modulate(const std::vector<uint8_t>& bits, const ModConfig& cfg, std::vector<cf>& out) {
    const int bps = bits_per_symbol(cfg.mod);
    const size_t nsym = bits.size() / (size_t)bps;
    out.clear();
    if (nsym == 0 || cfg.rate <= 0 || cfg.baud <= 0) return 0;

    const int gen_sps = 8;
    const double rate_gen = cfg.baud * gen_sps * (1.0 + cfg.ppm * 1e-6);

    std::vector<cf> ti(nsym * gen_sps, cf(0, 0)), tq(nsym * gen_sps, cf(0, 0));
    for (size_t k = 0; k < nsym; ++k) {
        float si, sq;
        if (bps == 1) { si = bits[k] ? -1.0f : 1.0f; sq = 0.0f; }
        else { si = bits[2 * k] ? -1.0f : 1.0f; sq = bits[2 * k + 1] ? -1.0f : 1.0f; }
        ti[k * gen_sps] = cf(si, 0);
        // OQPSK: Q is late by half a symbol.
        const size_t off = (cfg.mod == "oqpsk") ? (size_t)(gen_sps / 2) : 0;
        if (k * gen_sps + off < tq.size()) tq[k * gen_sps + off] = cf(sq, 0);
    }

    const std::vector<float> rrc = design_rrc(cfg.rrc_alpha, gen_sps, cfg.rrc_span);
    std::vector<cf> bi, bq;
    convolve_same(ti, rrc, bi);
    convolve_same(tq, rrc, bq);

    const size_t nout = (size_t)((double)nsym / cfg.baud * cfg.rate);
    out.assign(nout, cf(0, 0));

    const double step = rate_gen / cfg.rate;
    const double amp_i = std::pow(10.0, cfg.iq_gain_db / 20.0);
    const double ph = cfg.iq_phase_deg * kPi / 180.0;

    for (size_t n = 0; n < nout; ++n) {
        const double pos = (double)n * step;
        const long i1 = (long)pos;
        const double f = pos - (double)i1;

        auto sample_at = [&](const std::vector<cf>& b, bool real_part) {
            double y[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const long idx = i1 - 1 + k;
                if (idx < 0 || idx >= (long)b.size()) continue;
                y[k] = real_part ? (double)b[(size_t)idx].real() : (double)b[(size_t)idx].imag();
            }
            return cubic(y, f);
        };
        double i = sample_at(bi, true);
        double q = sample_at(bq, true);   // bq is real-carrying

        const double i2 = i * std::cos(ph * 0.5) - q * std::sin(ph * 0.5);
        const double q2 = i * std::sin(ph * 0.5) + q * std::cos(ph * 0.5);

        const double ph_c = 2.0 * kPi * cfg.offset_hz * (double)n / cfg.rate;
        const double c = std::cos(ph_c), s = std::sin(ph_c);
        out[n] = cf((float)(i2 * c - q2 * s), (float)(i2 * s + q2 * c));
    }
    return nsym;
}

double add_awgn(std::vector<cf>& x, double snr_db, double sps, double mf_energy, unsigned seed) {
    if (x.empty()) return 0.0;
    double p = 0.0;
    for (const auto& v : x) p += std::norm(v);
    p /= (double)x.size();
    const double snr = std::pow(10.0, snr_db / 10.0);
    // sigma^2 = P_s * fs / (B * snr); sps is fs/baud and mf_energy carries (1+alpha).
    const double sigma = std::sqrt(std::max(p, 1e-30) * std::max(sps, 1e-9) /
                                   std::max(snr * std::max(mf_energy, 1e-9), 1e-30));
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.0f, (float)sigma);
    for (auto& v : x) v += cf(d(rng), d(rng));
    return sigma;
}

}  // namespace eu
