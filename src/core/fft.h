// Radix-2 DIT, no fftw. Plans cached per size.
#pragma once

#include <complex>
#include <vector>

#include "core/common.h"

namespace eu {

class Fft {
public:
    explicit Fft(size_t n);
    size_t size() const { return n_; }

    void forward(cf* x) const;
    void inverse(cf* x) const;

    void power_spectrum(const cf* in, size_t nin, std::vector<double>& out) const;

    // Rotates so 0 Hz is centred.
    void fftshift(cf* x) const;
    void fftshift(double* x) const;

private:
    void transform(cf* x, bool inverse) const;
    size_t n_ = 0;
    std::vector<size_t> rev_;
    std::vector<cf> tw_;
};

std::vector<float> hann(size_t n);

// Fractional bin offset from a parabolic fit.
double parabolic_offset(const double* p, size_t n, size_t idx);

}  // namespace eu
