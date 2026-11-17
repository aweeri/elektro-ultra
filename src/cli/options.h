#pragma once

#include <string>
#include <vector>

#include "core/io.h"

namespace eu {

struct Options {
    std::string cmd;                  // decode | probe | synth | selftest | bench
    std::vector<std::string> inputs;

    SampleFormat fmt = SampleFormat::Auto;
    std::string  type = "auto";       // lrit | hrit | ggak | auto
    double       rate = 0.0;
    double       center = 0.0;

    double      baud = 0.0;
    std::string mod = "auto";
    bool        carrier_auto = true;
    double      carrier = 0.0;
    bool        invert = false;
    std::string polarity = "auto";
    int         sps = 4;
    double      rrc_alpha = 0.5;
    int         rrc_span = 16;
    bool        pll_bw_set = false;    // 0 means loop off, so presence is tracked
    bool        pll_bw_acq_set = false;
    double      pll_bw = 0.0015;
    double      pll_bw_acq = 0.003;
    double      clock_bw = 0.0;        // 0 = auto (~1 kHz)
    std::string agc = "auto";
    double      agc_ref = 0.6;
    std::string dc = "auto";
    std::string iqbal = "auto";
    double      time_offset = 0.0;    // seconds to skip

    int  cadu_bits = 8192;
    int  rs_depth = 4;
    bool derand = true;
    bool erasure = true;
    bool mv = true;
    bool dd_refine = true;
    bool sova = false;

    int         threads = 3;
    std::string out;
    bool        images = true;
    std::string png_depth = "auto";

    std::string dump_soft;
    std::string dump_bits;
    std::string dump_cadu;
    std::string stats;

    bool json = false;
    bool quiet = false;
    bool verbose = false;
    bool progress = false;
    bool json_stdout = false;

    double      max_seconds = 0.0;
    std::string stop_after;

    bool        gui = false;
    int         gui_frames = 0;
    std::string gui_out;
    double      gui_scale = 1.0;

    double synth_seconds = 1.0;
    double synth_snr = 10.0;
    double synth_offset = 0.0;
    double synth_ppm = 0.0;
    double synth_iq_gain_db = 0.0;
    double synth_iq_phase_deg = 0.0;
    unsigned synth_seed = 1;
    bool   synth_full_chain = false;
    std::string synth_pattern = "random";

    std::string manifest;
};

// False on a parse error; the message is already printed.
bool parse_options(int argc, char** argv, Options& o, std::string& err);
void print_help(const char* argv0);

// Fills demod/fec fields from the transmission type.
void apply_type_preset(Options& o);

}  // namespace eu
