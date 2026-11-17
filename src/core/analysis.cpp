#include "core/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "core/fft.h"

namespace eu {

static size_t pow2_ge(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Hann-windowed averaged PSD, centre bin at 0 Hz.
static void avg_psd(const cf* x, size_t n, size_t nfft, size_t max_blocks,
                    std::vector<double>& psd) {
    Fft fft(nfft);
    std::vector<float> win = hann(nfft);
    std::vector<cf> buf(nfft);
    std::vector<double> acc(nfft, 0.0), one;
    const size_t blocks = std::min(max_blocks, n / nfft);
    if (blocks == 0) { psd.assign(nfft, 0.0); return; }
    for (size_t b = 0; b < blocks; ++b) {
        const cf* src = x + b * nfft;
        for (size_t i = 0; i < nfft; ++i) buf[i] = src[i] * win[i];
        fft.forward(buf.data());
        fft.fftshift(buf.data());
        for (size_t i = 0; i < nfft; ++i) acc[i] += (double)std::norm(buf[i]);
    }
    double wsum = 0.0;
    for (float w : win) wsum += (double)w;
    const double norm = 1.0 / (wsum * wsum * 0.25);
    psd.resize(nfft);
    for (size_t i = 0; i < nfft; ++i) psd[i] = acc[i] / (double)blocks * norm;
}

static double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

double estimate_symbol_rate(const cf* x, size_t n, double fs, double& strength_db_out,
                            double min_hz, double max_hz) {
    strength_db_out = 0.0;
    if (n < 4096 || fs <= 0) return 0.0;
    const size_t nfft = pow2_ge(std::min<size_t>(n, 1u << 19));
    if (n < nfft) return 0.0;

    std::vector<cf> e(nfft);
    std::vector<float> win = hann(nfft);
    double mean = 0.0;
    for (size_t i = 0; i < nfft; ++i) mean += std::norm(x[i]);
    mean /= (double)nfft;
    for (size_t i = 0; i < nfft; ++i)
        e[i] = cf((float)((std::norm(x[i]) - mean) * win[i]), 0.0f);

    Fft fft(nfft);
    fft.forward(e.data());
    std::vector<double> p(nfft);
    for (size_t i = 0; i < nfft; ++i) p[i] = (double)std::norm(e[i]);

    const double bin_hz = fs / (double)nfft;
    const size_t lo = (size_t)std::max(1.0, min_hz / bin_hz);
    const size_t hi = (size_t)std::min((double)nfft / 2.0 - 2.0,
                                       (max_hz > 0 ? max_hz : fs * 0.48) / bin_hz);
    if (hi <= lo + 4) return 0.0;

    size_t best = lo;
    for (size_t i = lo; i <= hi; ++i) if (p[i] > p[best]) best = i;
    std::vector<double> band(p.begin() + (long)lo, p.begin() + (long)hi + 1);
    const double med = median_of(band);
    strength_db_out = 10.0 * std::log10(std::max(p[best], 1e-30) / std::max(med, 1e-30));
    if (strength_db_out < 6.0) return 0.0;

    const double frac = parabolic_offset(p.data(), nfft, best);
    double f = ((double)best + frac) * bin_hz;
    const size_t h2 = (size_t)std::llround(2.0 * f / bin_hz);
    if (h2 > lo && h2 + 1 < hi && best > lo + 2) {
        const double f2 = (double)best * bin_hz;
        const size_t half = (size_t)std::llround(0.5 * f2 / bin_hz);
        if (half > lo && half + 1 < hi && p[half] > p[best] * 0.35) {
            const double frac2 = parabolic_offset(p.data(), nfft, half);
            f = ((double)half + frac2) * bin_hz;
        }
    }
    return f;
}

ProbeResult probe_samples(const cf* x, size_t n, double fs, double expected_baud) {
    ProbeResult r;
    if (n < 4096 || fs <= 0) return r;

    double p = 0.0;
    cd dc(0, 0);
    size_t clipped = 0;
    for (size_t i = 0; i < n; ++i) {
        const double re = x[i].real(), im = x[i].imag();
        p += re * re + im * im;
        dc += cd(re, im);
        if (std::fabs(re) > 0.999 || std::fabs(im) > 0.999) ++clipped;
    }
    const double rms = std::sqrt(p / (double)n);
    dc /= (double)n;
    r.rms = rms;
    r.rms_dbfs = db(rms);
    r.clip_frac = (double)clipped / (double)n;
    r.dc_mag = rms > 0 ? std::abs(dc) / rms : 0.0;

    const size_t nfft = 1 << 15;
    std::vector<double> psd;
    avg_psd(x, n, nfft, 64, psd);
    std::vector<double> pdb(psd.size());
    for (size_t i = 0; i < psd.size(); ++i) pdb[i] = 10.0 * std::log10(std::max(psd[i], 1e-30));
    const double bin_hz = fs / (double)nfft;
    const size_t centre = nfft / 2;

    std::vector<double> all = pdb;
    const double floor_db = median_of(all);
    r.noise_floor_dbfs = floor_db;

    const size_t guard = (size_t)std::max(2.0, 2000.0 / bin_hz);
    double dc_peak = -1e9;
    for (size_t i = centre - guard; i <= centre + guard && i < nfft; ++i)
        dc_peak = std::max(dc_peak, pdb[i]);
    r.dc_spur_db = dc_peak - floor_db;

    size_t pk = 0;
    double pkv = -1e9;
    for (size_t i = 0; i < nfft; ++i) {
        if (i + guard >= centre && i <= centre + guard) continue;
        if (pdb[i] > pkv) { pkv = pdb[i]; pk = i; }
    }
    r.peak_db = pkv - floor_db;
    r.peak_offset_hz = ((double)pk - (double)centre) * bin_hz;

    const size_t box = (size_t)std::max(1.0, 4000.0 / bin_hz);
    std::vector<double> sm(nfft, 0.0);
    {
        double acc = 0.0;
        for (size_t i = 0; i < nfft; ++i) {
            acc += pdb[i];
            if (i >= box) acc -= pdb[i - box];
            sm[i] = acc / (double)std::min(i + 1, box);
        }
    }
    // +1.2 dB with 100 kHz gap merging: a weak link lifts the floor by only 2-4 dB and a +3 dB threshold shatters the band into spur-sized fragments.
    const double thr = floor_db + 1.2;
    const size_t gap_max = (size_t)std::max(2.0, 100000.0 / bin_hz);
    std::vector<std::pair<size_t, size_t>> runs;
    size_t i = 0;
    while (i < nfft) {
        if (sm[i] > thr) {
            size_t j = i;
            while (j < nfft && sm[j] > thr) ++j;
            if (j - i >= 4 && j < nfft - 1) {
                if (!runs.empty() && i - runs.back().second <= gap_max)
                    runs.back().second = j;
                else
                    runs.emplace_back(i, j);
            }
            i = j;
        } else {
            ++i;
        }
    }
    // Score by integrated excess power, not peak: a 20 dB spur carries no power.
    long pick = -1;
    double best_score = -1.0;
    const double flin = std::pow(10.0, floor_db / 10.0);
    for (size_t k = 0; k < runs.size(); ++k) {
        double score = 0.0;
        for (size_t j = runs[k].first; j <= runs[k].second; ++j)
            score += std::max(0.0, std::pow(10.0, pdb[j] / 10.0) - flin);
        if (score > best_score) { best_score = score; pick = (long)k; }
    }
    if (pick >= 0) {
        const size_t a = runs[(size_t)pick].first, b = runs[(size_t)pick].second;
        r.band_found = true;
        r.band_lo_hz = ((double)a - (double)centre) * bin_hz;
        r.band_hi_hz = ((double)b - (double)centre) * bin_hz;
        r.band_width_hz = r.band_hi_hz - r.band_lo_hz;
        double wsum = 0.0, fsum = 0.0, sig = 0.0;
        for (size_t k = a; k <= b; ++k) {
            const double lin = std::pow(10.0, pdb[k] / 10.0);
            const double fl = std::pow(10.0, floor_db / 10.0);
            const double w = std::max(0.0, lin - fl);
            wsum += w;
            fsum += w * ((double)k - (double)centre) * bin_hz;
            sig += w;
        }
        r.centroid_hz = wsum > 0 ? fsum / wsum : 0.0;
        const double noise = std::pow(10.0, floor_db / 10.0) * (double)(b - a + 1);
        r.inband_snr_db = 10.0 * std::log10(std::max(sig, 1e-30) / std::max(noise, 1e-30));
    }

    r.symbol_rate = estimate_symbol_rate(x, n, fs, r.rate_strength_db,
                                         expected_baud > 0 ? expected_baud * 0.85 : 20000.0,
                                         expected_baud > 0 ? expected_baud * 1.15 : 0.0);
    r.rate_found = r.symbol_rate > 0.0;

    {
        std::vector<cf> sq(1u << 17);
        const size_t m = std::min<size_t>(n, sq.size());
        cd mean(0, 0);
        for (size_t k = 0; k < m; ++k) mean += cd(x[k]);
        mean /= (double)m;
        for (size_t k = 0; k < m; ++k) {
            const cf v = x[k] - cf((float)mean.real(), (float)mean.imag());
            sq[k] = v * v;
        }
        Fft fft(sq.size());
        fft.forward(sq.data());
        std::vector<double> ps(sq.size());
        for (size_t k = 0; k < sq.size(); ++k) ps[k] = (double)std::norm(sq[k]);
        ps[0] = 0.0;
        const double med = median_of(ps);
        const double mx = *std::max_element(ps.begin(), ps.end());
        r.square_line_db = 10.0 * std::log10(std::max(mx, 1e-30) / std::max(med, 1e-30));

        for (size_t k = 0; k < m; ++k) {
            const cf v = x[k] - cf((float)mean.real(), (float)mean.imag());
            const cf v2 = v * v;
            sq[k] = v2 * v2;
        }
        fft.forward(sq.data());
        for (size_t k = 0; k < sq.size(); ++k) ps[k] = (double)std::norm(sq[k]);
        ps[0] = 0.0;
        const double med4 = median_of(ps);
        const double mx4 = *std::max_element(ps.begin(), ps.end());
        r.fourth_line_db = 10.0 * std::log10(std::max(mx4, 1e-30) / std::max(med4, 1e-30));
    }
    if (r.square_line_db > 20.0 && r.square_line_db > r.fourth_line_db + 3.0) r.mod_guess = "bpsk";
    else if (r.fourth_line_db > 15.0) r.mod_guess = "qpsk/oqpsk";
    else r.mod_guess = "unknown";

    r.looks_empty = (r.rms_dbfs < -40.0) || (r.band_found && r.inband_snr_db < 1.0) ||
                    (!r.band_found && r.peak_db < 6.0);
    return r;
}

std::string ProbeResult::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "rms %.1f dBFS  band %.0f..%.0f kHz  Rs %.0f Bd (%.1f dB)  cen %+.1f kHz  "
                  "snr %.1f dB  dc %.1f dB  x2 %.1f dB  x4 %.1f dB  %s%s",
                  rms_dbfs, band_lo_hz / 1e3, band_hi_hz / 1e3, symbol_rate, rate_strength_db,
                  centroid_hz / 1e3, inband_snr_db, dc_spur_db, square_line_db, fourth_line_db,
                  mod_guess.c_str(), looks_empty ? "  [EMPTY]" : "");
    return std::string(buf);
}

}  // namespace eu
