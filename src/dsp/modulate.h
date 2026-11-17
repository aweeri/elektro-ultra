#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"

namespace eu {

struct ModConfig {
    std::string mod = "bpsk";
    double baud = 294000.0;
    double rate = 3000000.0;
    double rrc_alpha = 0.5;
    int    rrc_span = 16;
    double snr_db = 20.0;
    double offset_hz = 0.0;
    double ppm = 0.0;
    double iq_gain_db = 0.0;
    double iq_phase_deg = 0.0;
    unsigned seed = 1;
    double amplitude = 0.25;
};

// Writes cfg.rate samples from 0/1 bits; returns symbols transmitted.
size_t modulate(const std::vector<uint8_t>& bits, const ModConfig& cfg, std::vector<cf>& out);

std::vector<uint8_t> random_bits(size_t nsym, int bits_per_symbol, unsigned seed);

int bits_per_symbol(const std::string& mod);

// Adds noise for Es/N0 snr_db; returns sigma used.
double add_awgn(std::vector<cf>& x, double snr_db, double sps, double matched_filter_energy,
                unsigned seed);

}  // namespace eu
