// Front-end sanity: resampler output rate, RRC gain, and the eye at 4 sps.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/common.h"
#include "dsp/filters.h"
#include "dsp/modulate.h"
#include "dsp/resample.h"

using namespace eu;

int main() {
    ModConfig mc;
    mc.mod = "bpsk";
    mc.baud = kLritBaud;
    mc.rate = 3e6;
    mc.snr_db = 40.0;
    mc.seed = 3;

    const size_t nsym = (size_t)(kLritBaud * 0.05);
    std::vector<uint8_t> bits = random_bits(nsym, 1, mc.seed);
    std::vector<cf> x;
    modulate(bits, mc, x);
    add_awgn(x, mc.snr_db, mc.rate / mc.baud, 1.5, 99u);

    const double fs_out = 4 * kLritBaud;
    Resampler rs;
    if (!rs.configure(mc.rate, fs_out, 80.0, 12)) { std::printf("resampler failed\n"); return 1; }
    std::printf("resampler L=%d M=%d taps/phase=%zu ratio=%.6f (want %.6f)\n",
                (int)std::lround(fs_out), (int)std::lround(mc.rate), rs.taps_per_phase(),
                rs.ratio(), fs_out / mc.rate);

    std::vector<cf> y;
    rs.process(x.data(), x.size(), y);
    std::printf("in %zu -> out %zu, ratio %.6f\n", x.size(), y.size(),
                (double)y.size() / (double)x.size());

    auto taps = design_rrc(0.5, 4, 16);
    double sum = 0;
    for (float t : taps) sum += t;
    std::printf("rrc: %zu taps, sum %.4f\n", taps.size(), sum);

    Fir fir;
    fir.set_taps(taps);
    fir.process(y.data(), y.data(), y.size());

    std::printf("first x: ");
    for (int i = 0; i < 6; ++i) std::printf("%+.3f%+.3fj ", x[i].real(), x[i].imag());
    std::printf("\nfirst y: ");
    for (int i = 0; i < 6; ++i) std::printf("%+.3f%+.3fj ", y[i].real(), y[i].imag());
    std::printf("\nfirst f: ");
    for (int i = 200; i < 216; ++i) std::printf("%+.3f ", y[i].real());
    std::printf("\n");

    for (int pp = 0; pp < 4; ++pp) {
        int hist[12] = {0};
        size_t nn = 0;
        for (size_t i = (size_t)pp; i < y.size(); i += 4) {
            int b = (int)std::floor((y[i].real() + 0.36) / 0.06);
            if (b < 0) b = 0;
            if (b > 11) b = 11;
            hist[b]++;
            ++nn;
        }
        std::printf("  ph%d hist -0.36..+0.36:", pp);
        for (int k = 0; k < 12; ++k) std::printf("%4.0f%%", 100.0 * hist[k] / (double)nn);
        std::printf("\n");
    }

    for (int p = 0; p < 4; ++p) {
        double acc = 0, ar = 0, ar2 = 0; size_t n = 0, strong = 0;
        for (size_t i = (size_t)p; i < y.size(); i += 4) {
            acc += std::norm(y[i]);
            const double r = std::fabs((double)y[i].real());
            ar += r; ar2 += r * r;
            if (r > 0.5) ++strong;
            ++n;
        }
        const double mr = ar / (double)n;
        const double sd = std::sqrt(std::max(0.0, ar2 / (double)n - mr * mr));
        std::printf("  phase %d: mean|y|^2 %.4f  mean|Re| %.3f sd %.3f  frac>0.5 %.2f\n",
                    p, acc / (double)n, mr, sd, (double)strong / (double)n);
    }
    // Real/imag balance is a direct IQ-imbalance and carrier check.
    double sr = 0, si = 0;
    for (const auto& v : y) { sr += v.real() * v.real(); si += v.imag() * v.imag(); }
    std::printf("  I power %.4f, Q power %.4f\n", sr / y.size(), si / y.size());
    return 0;
}
