// DC -> NCO -> resample -> RRC -> OQPSK align -> AGC -> Gardner -> Costas -> soft demap.
#include "dsp/demod.h"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace eu {

const char* mod_name(Mod m) {
    switch (m) {
        case Mod::Bpsk:  return "bpsk";
        case Mod::Qpsk:  return "qpsk";
        case Mod::Oqpsk: return "oqpsk";
    }
    return "?";
}

bool mod_from_name(const std::string& s, Mod& out) {
    if (s == "bpsk" || s == "BPSK") { out = Mod::Bpsk; return true; }
    if (s == "qpsk" || s == "QPSK") { out = Mod::Qpsk; return true; }
    if (s == "oqpsk" || s == "OQPSK" || s == "sqpsk" || s == "uqpsk") {
        out = Mod::Oqpsk; return true;
    }
    return false;
}

namespace {

// Type-2 loop for a given noise bandwidth; without the normalisation the gain is ~30x off and the loop does not track.
void pi_gains(double bn, double& kp, double& ki) {
    const double zeta = 0.70710678;
    const double theta = bn / (zeta + 0.25 / zeta);
    const double den = 1.0 + 2.0 * zeta * theta + theta * theta;
    kp = 4.0 * zeta * theta / den;
    ki = 4.0 * theta * theta / den;
}

constexpr size_t kPrimeSamples = 1u << 19;   // long enough for the M-th power line

}  // namespace

void DemodChain::init_loops() {
    pi_gains(cfg_.clock_bw, kp_t_, ki_t_);
    pi_gains(cfg_.pll_bw, kp_c_, ki_c_);
    pi_gains(std::max(cfg_.pll_bw, cfg_.pll_bw_acq), kp_ca_, ki_ca_);
    t_next_ = 0.0;
    n_pushed_ = 0;
    y_prev_ = cf(0, 0);
    have_prev_ = false;
    tim_integ_ = 0.0;
    phase_ = 0.0;
    integ_ = 0.0;
    agc_p_ = 0.0;
    agc_g_ = 1.0;
    qpos_ = 0;
    evm_acc_ = 0.0;
    evm_n_ = 0;
    const_.clear();
    const_pos_ = 0;
    st_ = DemodStats{};
}

bool DemodChain::configure(const DemodConfig& cfg, double fs_in, std::string* err) {
    cfg_ = cfg;
    fs_in_ = fs_in;
    ok_ = false;
    if (cfg.sps < 2 || cfg.sps > 16) {
        if (err) *err = "sps must be 2..16";
        return false;
    }
    if (cfg.mod == Mod::Oqpsk && ((cfg.sps & 1) != 0)) {
        if (err) *err = "oqpsk needs an even sps (the Q rail is delayed by Ts/2)";
        return false;
    }
    fs_out_ = cfg.baud * (double)cfg.sps;
    if (!resamp_.configure(fs_in, fs_out_, 80.0, 12)) {
        if (err) {
            *err = "resampler cannot realise " + std::to_string(fs_in) + " -> " +
                   std::to_string(fs_out_);
        }
        return false;
    }
    rrc_.set_taps(design_rrc(cfg.rrc_alpha, cfg.sps, 16));
    dc_.enabled = cfg.dc_block;
    dc_.reset();
    nco_.reset();

    qd_ = (cfg.mod == Mod::Oqpsk && cfg.oqpsk_align) ? cfg.sps / 2 : 0;
    qdelay_.assign((size_t)std::max(qd_, 1), 0.0f);

    dl_.reset(64);
    pending_.clear();
    init_loops();
    ok_ = true;
    return true;
}

void DemodChain::process(const cf* x, size_t n, std::vector<int8_t>& out) {
    if (!ok_ || n == 0) return;
    if (st_.primed) {
        run_block(x, n, out);
        return;
    }
    pending_.insert(pending_.end(), x, x + n);
    if (pending_.size() >= kPrimeSamples) prime_and_process(out);
}

void DemodChain::flush(std::vector<int8_t>& out) {
    if (!ok_) return;
    if (!st_.primed) {
        if (pending_.size() >= 8192) prime_and_process(out);
        else pending_.clear();
        return;
    }
}

void DemodChain::prime_and_process(std::vector<int8_t>& out) {
    const size_t n = pending_.size();
    if (n < 8192) return;

    // DC-blocked copy: LO leakage is 40+ dB above the signal on the cs8 captures.
    b1_.resize(n);
    std::memcpy(b1_.data(), pending_.data(), n * sizeof(cf));
    dc_.process(b1_.data(), b1_.data(), n);

    double width = 0.0;
    // Window must cover the occupied band (1.5*Rs for RRC 0.5) or the centroid biases towards DC.
    const double cen_win = std::max(cfg_.search_hz, 0.9 * cfg_.baud);
    const double cen = band_centroid(b1_.data(), n, fs_in_, cen_win, &width);
    const int order = (cfg_.mod == Mod::Bpsk) ? 2 : 4;
    double strength = 0.0;
    // Search the whole range, not a window around the centroid: asymmetry pulls the centroid tens of kHz and a narrow window misses the M-th power line.
    const double acq_span = std::max(5000.0, std::min(cfg_.search_hz, 0.6 * fs_in_));
    double fine = acquire_carrier(b1_.data(), n, fs_in_, 0.0, order, order * acq_span, strength);
    if (strength < 6.0) {
        // No convincing line (common for OQPSK): the centroid beats a noise peak.
        fine = cen;
    }

    st_.carrier_hz = cfg_.carrier_auto ? fine : cfg_.carrier_hz;
    st_.acq_strength_db = strength;
    st_.rms_in = 0.0;
    for (size_t i = 0; i < n; ++i) st_.rms_in += std::norm(pending_[i]);
    st_.rms_in = std::sqrt(st_.rms_in / (double)n);

    nco_.set_freq_hz(st_.carrier_hz, fs_in_);
    nco_.set_phase(0.0);
    dc_.reset();
    st_.primed = true;

    run_block(pending_.data(), n, out);
    pending_.clear();
    pending_.shrink_to_fit();
}

void DemodChain::run_block(const cf* x, size_t n, std::vector<int8_t>& out) {
    static const bool prof = std::getenv("EU_PROFILE") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    auto mark = [&](int stage) {
        if (!prof) return;
        const auto t = std::chrono::steady_clock::now();
        t_stage_[stage] += std::chrono::duration<double>(t - t0).count();
        t0 = t;
    };
    b1_.resize(n);
    if (cfg_.invert) {
        for (size_t i = 0; i < n; ++i) b1_[i] = std::conj(x[i]);
    } else {
        std::memcpy(b1_.data(), x, n * sizeof(cf));
    }

    static const bool no_dc = std::getenv("EU_NO_DC") != nullptr;
    if (no_dc) dc_.enabled = false;
    dc_.process(b1_.data(), b1_.data(), n);
    mark(0);
    nco_.process(b1_.data(), b1_.data(), n);
    mark(1);

    static const char* dump_b1 = std::getenv("EU_DUMP_B1");
    if (const char* dp = dump_b1) {
        if (FILE* f = std::fopen(dp, "ab")) {
            std::fwrite(b1_.data(), sizeof(cf), b1_.size(), f);
            std::fclose(f);
        }
    }

    b2_.clear();
    resamp_.process(b1_.data(), n, b2_);
    mark(2);
    if (b2_.empty()) return;

    static const char* dump_br = std::getenv("EU_DUMP_BR");
    if (const char* dp = dump_br) {
        if (FILE* f = std::fopen(dp, "ab")) {
            std::fwrite(b2_.data(), sizeof(cf), b2_.size(), f);
            std::fclose(f);
        }
    }

    rrc_.process(b2_.data(), b2_.data(), b2_.size());
    mark(3);

    if (qd_ > 0 && !qdelay_.empty()) {
        // The transmitter sends Q late, so I is delayed by Ts/2; delaying Q instead puts the rails a whole symbol apart.
        const size_t qn = qdelay_.size();
        for (size_t i = 0; i < b2_.size(); ++i) {
            const float v = cfg_.oqpsk_delay_q ? b2_[i].imag() : b2_[i].real();
            const float dv = qdelay_[qpos_];
            qdelay_[qpos_] = v;
            qpos_ = (qpos_ + 1) % qn;
            b2_[i] = cfg_.oqpsk_delay_q ? cf(b2_[i].real(), dv) : cf(dv, b2_[i].imag());
        }
    }

    mark(4);

    // Power is measured even with the gain off: the MER readout needs it, or an AGC-off decode reports MER 4.8 dB on a 22 dB signal.
    {
        double acc = 0.0;
        for (size_t i = 0; i < b2_.size(); ++i) acc += std::norm(b2_[i]);
        const double blk = acc / (double)b2_.size();
        if (agc_p_ <= 0.0) {
            agc_p_ = blk;
        } else {
            const double a = 0.15;
            agc_p_ = (1.0 - a) * agc_p_ + a * blk;
        }
        if (agc_p_ < 1e-20) agc_p_ = 1e-20;
        agc_g_ = 1.0 / std::sqrt(agc_p_);
    }
    evm_norm_ = cfg_.agc ? 1.0 : agc_g_;
    if (cfg_.agc) {
        const float gf = (float)agc_g_;
        for (size_t i = 0; i < b2_.size(); ++i) b2_[i] *= gf;
    }
    mark(5);

    static const char* dump_b2 = std::getenv("EU_DUMP_B2");
    if (const char* dp = dump_b2) {
        if (FILE* f = std::fopen(dp, "ab")) {
            std::fwrite(b2_.data(), sizeof(cf), b2_.size(), f);
            std::fclose(f);
        }
    }

    const double sps = (double)cfg_.sps;
    if (!have_prev_ && t_next_ == 0.0) {
        // Maximum mean |y|^2 is the eye centre; it also puts the Gardner loop on the stable side of its T/2 false zero.
        const size_t N = b2_.size();
        const int step_div = 8;
        double best = -1.0;
        double best_off = 0.0;
        for (int k = 0; k < cfg_.sps * step_div; ++k) {
            const double off = (double)k / (double)step_div;
            double acc = 0.0;
            size_t cnt = 0;
            for (double p = off; p + 1.0 < (double)N;
                 p += (double)cfg_.sps) {
                const size_t i = (size_t)p;
                const float f = (float)(p - (double)i);
                const cf v = b2_[i] * (1.0f - f) + b2_[i + 1] * f;
                acc += std::norm(v);
                ++cnt;
            }
            const double m = cnt ? acc / (double)cnt : 0.0;
            if (m > best) { best = m; best_off = off; }
        }
        t_next_ = best_off;
        st_.timing_phase = best_off;
        if (std::getenv("EU_DEMOD_DEBUG")) {
            std::fprintf(stderr, "acq: best_off %.3f mean|y|^2 %.5f (N=%zu)\n", best_off, best, N);
        }
    }

    const double mid = sps * 0.5;
    const float scale = (float)cfg_.soft_scale;

    for (size_t i = 0; i < b2_.size(); ++i) {
        dl_.push(b2_[i]);
        ++n_pushed_;

        while (t_next_ <= (double)(n_pushed_ - 3)) {
            const double d = (double)(n_pushed_ - 1) - t_next_;
            if (d < 0.0) break;
            const cf y = dl_.at(d);
            const cf ymid = dl_.at(d + mid);

            // Normalised by local power so the loop gain is AGC independent.
            double e_dbg = 0.0;
            if (have_prev_) {
                const cf diff = y - y_prev_;
                const double e = (diff * std::conj(ymid)).real() /
                                 (std::norm(y) + std::norm(ymid) + 1e-12);
                e_dbg = e;
                tim_integ_ += ki_t_ * e;
                const double corr = kp_t_ * e + tim_integ_;
                corr_sum_ += corr;
                ++corr_n_;
                // e > 0 means the sample is late (past the eye centre).
                t_next_ += sps - corr;
            } else {
                t_next_ += sps;
            }
            y_prev_ = y;
            have_prev_ = true;


            // Measured on the derotated symbol: on the raw one the loop tracks the absolute phase and never settles.
            cf sym = y * cf((float)std::cos(phase_), (float)-std::sin(phase_));
            double pe = 0.0;
            if (cfg_.mod == Mod::Bpsk) {
                const double re = sym.real(), im = sym.imag();
                const double sgn = (re >= 0.0) ? 1.0 : -1.0;
                pe = (sgn * im) / (re * re + im * im + 1e-12);
            } else {
                const double re = sym.real(), im = sym.imag();
                const double sr = (re >= 0.0) ? 1.0 : -1.0;
                const double si = (im >= 0.0) ? 1.0 : -1.0;
                pe = (sr * im - si * re) / (std::fabs(re) + std::fabs(im) + 1e-12);
            }
            // Wide loop to acquire, narrow to track; the integrator carries the frequency estimate across the switch.
            const bool acq = st_.symbols < (uint64_t)cfg_.acq_symbols;
            const double kp = acq ? kp_ca_ : kp_c_;
            const double ki = acq ? ki_ca_ : ki_c_;
            integ_ += ki * pe;
            phase_ = wrap_pi(phase_ + integ_ + kp * pe);

            static const bool dbg = std::getenv("EU_DEMOD_DEBUG") != nullptr;
            if (dbg && st_.symbols < 6000 && (st_.symbols % 500) == 0) {
                std::fprintf(stderr,
                             "sym %llu t=%.3f d=%.3f e=%+.4f |y|=%.3f |mid|=%.3f "
                             "ph=%+.3f sym=%+.3f%+.3fj\n",
                             (unsigned long long)st_.symbols, t_next_, d, e_dbg,
                             std::abs(y), std::abs(ymid), phase_,
                             sym.real(), sym.imag());
            }
            ++st_.symbols;

            if (st_.symbols > 200) {
                double dr = 0.0, di = 0.0;
                if (cfg_.mod == Mod::Bpsk) {
                    dr = (sym.real() >= 0.0) ? 1.0 : -1.0;
                } else {
                    dr = (sym.real() >= 0.0) ? 0.70710678 : -0.70710678;
                    di = (sym.imag() >= 0.0) ? 0.70710678 : -0.70710678;
                }
                // M2M4 is what SatDump reports as "SNR"; keeping it lets the two readouts be compared.
                {
                    const double p2 = std::norm(sym);
                    m2_acc_ += p2;
                    m4_acc_ += p2 * p2;
                }
                const double er = sym.real() * evm_norm_ - dr;
                const double ei = sym.imag() * evm_norm_ - di;
                evm_acc_ += er * er + ei * ei;
                evm_n_ += 1.0;
                if (const_.size() < 4096) const_.push_back(sym);
                else { const_[const_pos_] = sym; const_pos_ = (const_pos_ + 1) % 4096; }
            }

            // Scale 100 leaves headroom in int8 for the branch metrics.
            if (cfg_.mod == Mod::Bpsk) {
                const float v = clampf(sym.real() * scale, -127.0f, 127.0f);
                out.push_back((int8_t)std::lround(v));
            } else {
                const float vi = clampf(sym.real() * scale, -127.0f, 127.0f);
                const float vq = clampf(sym.imag() * scale, -127.0f, 127.0f);
                out.push_back((int8_t)std::lround(vi));
                out.push_back((int8_t)std::lround(vq));
            }
        }
    }

    if (evm_n_ > 0) {
        const double evm = std::sqrt(evm_acc_ / evm_n_);
        st_.evm_pct = evm * 100.0;
        st_.snr_db = evm > 1e-9 ? -20.0 * std::log10(evm) : 99.0;
        const double m2 = m2_acc_ / evm_n_;
        const double m4 = m4_acc_ / evm_n_;
        double sig = 2.0 * m2 * m2 - m4;
        sig = sig > 0.0 ? std::sqrt(sig) : 0.0;
        const double noi = m2 - sig;
        st_.m2m4_db = (sig > 0.0 && noi > 0.0) ? 10.0 * std::log10(sig / noi) : -99.0;
        st_.locked = (evm < 0.35) && (st_.symbols > 2000);
    }
    st_.bits = out.size();
    st_.timing_phase = t_next_ - std::floor(t_next_ / sps) * sps;
    st_.freq_err_hz = integ_ * cfg_.baud / (2.0 * kPi);
    st_.clock_ppm = corr_n_ ? corr_sum_ / ((double)corr_n_ * sps) * 1e6 : 0.0;
    st_.rms_out = std::sqrt(std::max(0.0, agc_p_));
}

}  // namespace eu
