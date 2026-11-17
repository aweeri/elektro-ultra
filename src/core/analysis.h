// Everything here is measured from the samples, never assumed.
#pragma once

#include <string>
#include <vector>

#include "core/common.h"

namespace eu {

struct ProbeResult {
    double rms = 0.0;
    double rms_dbfs = 0.0;
    double clip_frac = 0.0;
    double dc_mag = 0.0;         // |mean(x)| / rms
    double dc_spur_db = 0.0;     // DC bin over local median

    double noise_floor_dbfs = 0.0;   // median bin, dBFS
    double peak_offset_hz = 0.0;     // strongest non-DC bin
    double peak_db = 0.0;            // over median

    bool   band_found = false;
    double band_lo_hz = 0.0;         // -6 dB edges
    double band_hi_hz = 0.0;
    double band_width_hz = 0.0;
    double centroid_hz = 0.0;

    bool   rate_found = false;
    double symbol_rate = 0.0;
    double rate_strength_db = 0.0;   // line over local median

    double square_line_db = 0.0;     // x^2 tone -> BPSK
    double fourth_line_db = 0.0;     // x^4 tone -> QPSK
    std::string mod_guess;           // "bpsk" | "qpsk/oqpsk" | "unknown"

    double inband_snr_db = 0.0;

    bool   looks_empty = false;
    std::string summary() const;
};

// n should be >= 2^17 for a usable result.
ProbeResult probe_samples(const cf* x, size_t n, double fs, double expected_baud = 0.0);

// 0 if no cyclostationary line is found.
double estimate_symbol_rate(const cf* x, size_t n, double fs, double& strength_db_out,
                            double min_hz = 20000.0, double max_hz = 0.0);

}  // namespace eu
