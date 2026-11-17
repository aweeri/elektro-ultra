// FracDelay truth test: push a ramp (sample i has value i) and check at(t) returns (newest - t).
#include <cmath>
#include <cstdio>

#include "core/common.h"
#include "dsp/resample.h"

using namespace eu;

int main() {
    FracDelay dl;
    dl.reset(64);
    for (int i = 0; i < 100; ++i) dl.push(cf((float)i, 0));

    int bad = 0;
    for (int t = 0; t <= 20; ++t) {
        const double got = dl.at((double)t).real();
        const double want = 99.0 - t;
        if (std::fabs(got - want) > 1e-3) {
            std::printf("at(%d) = %.3f, want %.3f\n", t, got, want);
            ++bad;
        }
    }
    // Fractional: at(2.5) should sit between sample 97 and 96.
    const double f = dl.at(2.5).real();
    std::printf("at(2.5) = %.3f (want 96.5)\n", f);
    if (std::fabs(f - 96.5) > 0.15) ++bad;

    std::printf(bad ? "FAIL (%d)\n" : "ok\n", bad);
    return bad ? 1 : 0;
}
