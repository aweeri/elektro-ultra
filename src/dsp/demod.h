// Gardner runs before Costas: its error is rotation invariant.
#pragma once

#include <string>
#include <vector>

#include "core/common.h"
#include "dsp/filters.h"
#include "dsp/resample.h"

namespace eu {

enum class Mod { Bpsk = 0, Qpsk, Oqpsk };

const char* mod_name(Mod m);
bool mod_from_name(const std::string& s, Mod& out);

struct DemodConfig {
    Mod    mod = Mod::Bpsk;
    double baud = kLritBaud;
    int    sps = 4;
    double rrc_alpha = 0.5;
    double clock_bw = 0.010;     // /Rs
    double pll_bw = 0.010;       // /Rs
    double pll_bw_acq = 0.003;   // /Rs, acquisition
    int    acq_symbols = 40000;
    bool   dc_block = true;
    bool   agc = true;
    bool   invert = false;       // conjugate input
    bool   oqpsk_align = true;   // align the two OQPSK rails
    bool   oqpsk_delay_q = false; // false delays I (the transmitter sends Q late)
    bool   carrier_auto = true;
    double carrier_hz = 0.0;     // used when carrier_auto is false
    double search_hz = 40000.0;  // acquisition half-width
    double soft_scale = 100.0;
};

struct DemodStats {
    double   rms_in = 0.0;
    double   rms_out = 0.0;
    double   carrier_hz = 0.0;     // coarse offset, removed by the NCO
    double   acq_strength_db = 0.0;
    double   freq_err_hz = 0.0;    // residual, tracked by the Costas
    double   evm_pct = 0.0;
    double   snr_db = 0.0;
    double   m2m4_db = 0.0;        // SatDump-style readout
    double   timing_phase = 0.0;
    double   clock_ppm = 0.0;
    uint64_t symbols = 0;
    uint64_t bits = 0;
    bool     primed = false;
    bool     locked = false;
};

class DemodChain {
public:
    bool configure(const DemodConfig& cfg, double fs_in, std::string* err);

    // Appends int8 soft bits; may append nothing while priming.
    void process(const cf* x, size_t n, std::vector<int8_t>& out);
    void flush(std::vector<int8_t>& out);

    const DemodStats& stats() const { return st_; }
    const std::vector<cf>& constellation() const { return const_; }
    double fs_out() const { return fs_out_; }
    int    sps() const { return cfg_.sps; }

private:
    void prime_and_process(std::vector<int8_t>& out);
    void run_block(const cf* x, size_t n, std::vector<int8_t>& out);
    void init_loops();

    DemodConfig cfg_;
    double fs_in_ = 0.0, fs_out_ = 0.0;
    bool   ok_ = false;

    DcBlocker dc_;
    Nco       nco_;
    Resampler resamp_;
    Fir       rrc_;
    double    agc_p_ = 1.0;
    double    agc_g_ = 1.0;

    std::vector<float> qdelay_;
    size_t qpos_ = 0;
    int    qd_ = 0;

    FracDelay dl_;
    double t_next_ = 0.0;
    long long n_pushed_ = 0;
    cf     y_prev_{0, 0};
    bool   have_prev_ = false;
    double tim_integ_ = 0.0;
    double corr_sum_ = 0.0;
    uint64_t corr_n_ = 0;
    double kp_t_ = 0.0, ki_t_ = 0.0;

    double phase_ = 0.0;
    double integ_ = 0.0;
    double kp_c_ = 0.0, ki_c_ = 0.0;
    double kp_ca_ = 0.0, ki_ca_ = 0.0;

    std::vector<cf>     b1_, b2_, b3_, pending_;
    std::vector<int8_t> dummy_;
    std::vector<cf>     const_;
    size_t              const_pos_ = 0;

    // Filled only when EU_PROFILE is set.
    double t_stage_[8] = {};

public:
    const double* stage_times() const { return t_stage_; }

private:
    DemodStats st_;
    double m2_acc_ = 0.0, m4_acc_ = 0.0;
    double evm_norm_ = 1.0;
    double evm_acc_ = 0.0;
    uint64_t evm_n_ = 0;
    bool warned_odd_sps_ = false;
};

}  // namespace eu
