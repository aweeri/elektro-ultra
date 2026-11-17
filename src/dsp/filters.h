#pragma once

#include <vector>

#include "core/common.h"

namespace eu {

// RRC, span symbols each side, unity DC gain so +-1 in gives +-1 at decision.
std::vector<float> design_rrc(double alpha, int sps, int span);

// Cutoff in cycles/sample (0..0.5).
std::vector<float> design_lowpass(double cutoff, int ntaps, double beta);

// Kaiser beta for a stopband attenuation (dB).
double kaiser_beta(double atten_db);

class Fir {
public:
    void set_taps(std::vector<float> taps);
    // out may alias in.
    void process(const cf* in, cf* out, size_t n);
    void reset();
    size_t taps() const { return taps_.size(); }

private:
    std::vector<float> taps_;
    std::vector<cf> trev_;      // taps reversed, so window and taps run forwards
    std::vector<float> taps2_;  // doubled taps for the complex multiply
    std::vector<cf> hist_;      // last nt-1 inputs
    std::vector<cf> hb_;        // history + block
    size_t pos_ = 0;
};

// 'same' alignment, whole buffer.
void convolve_same(const std::vector<cf>& in, const std::vector<float>& taps,
                   std::vector<cf>& out);

}  // namespace eu
