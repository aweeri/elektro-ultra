// Resampler truth test: pass a complex tone through and measure amplitude and phase error against the exact expected waveform.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/common.h"
#include "dsp/resample.h"

using namespace eu;

int main() {
    const double fs_in = 3e6, fs_out = 4 * 294000.0;   // 1.176 MSPS
    const double f = 100000.0;
    const size_t n = 300000;
    std::vector<cf> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * f * (double)i / fs_in;
        x[i] = cf((float)std::cos(ph), (float)std::sin(ph));
    }

    Resampler rs;
    if (!rs.configure(fs_in, fs_out, 80.0, 12)) { std::printf("configure failed\n"); return 1; }
    std::vector<cf> y;
    rs.process(x.data(), x.size(), y);

    // The exact expected sample m is exp(j*2*pi*f*m/fs_out). Skip the ramp-in.
    const size_t skip = 500;
    double sum = 0, sum2 = 0;
    double re = 0, im = 0;
    size_t cnt = 0;
    for (size_t m = skip; m < y.size(); ++m) {
        const double ph = 2.0 * kPi * f * (double)m / fs_out;
        const cf want((float)std::cos(ph), (float)std::sin(ph));
        const cf d = y[m] * std::conj(want);
        re += d.real(); im += d.imag(); ++cnt;
        sum += std::abs(y[m]); sum2 += std::norm(y[m]);
    }
    const double mag = sum / (double)cnt;
    const double err = (std::atan2(im / (double)cnt, re / (double)cnt));
    double resid = 0;
    for (size_t m = skip; m < y.size(); ++m) {
        const double ph = 2.0 * kPi * f * (double)m / fs_out;
        const cf want((float)std::cos(ph), (float)std::sin(ph));
        resid += std::norm(y[m] - want * cf((float)std::cos(err), (float)std::sin(err)));
    }
    resid /= (double)cnt;
    std::printf("out %zu samples, |y| mean %.4f, rms %.4f, phase err %.4f rad, "
                "residual power %.5f (%.1f dB)\n",
                y.size(), mag, std::sqrt(sum2 / (double)cnt), err, resid,
                10.0 * std::log10(resid + 1e-12));
    return 0;
}
