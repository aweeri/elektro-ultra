#include "cli/options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/common.h"

namespace eu {

static bool need_float(const char* flag, const char* v, double& dst, std::string& err) {
    if (!v) { err = std::string(flag) + " needs a value"; return false; }
    char* end = nullptr;
    dst = std::strtod(v, &end);
    if (end == v) { err = std::string(flag) + ": not a number: " + v; return false; }
    return true;
}

void apply_type_preset(Options& o) {
    if (o.type == "lrit") {
        if (o.baud <= 0) o.baud = 294000.0;
        if (o.mod == "auto") o.mod = "bpsk";
        if (o.pll_bw == 0.0015) o.pll_bw = 0.0015;
    } else if (o.type == "hrit") {
        if (o.baud <= 0) o.baud = 1150000.0;
        if (o.mod == "auto") o.mod = "oqpsk";
        if (o.pll_bw == 0.0015) o.pll_bw = 0.0015;
    } else if (o.type == "ggak") {
        if (o.baud <= 0) o.baud = 5000.0;
        if (o.mod == "auto") o.mod = "bpsk";
    }
}

bool parse_options(int argc, char** argv, Options& o, std::string& err) {
    if (argc < 2) { err = "no command"; return false; }
    o.cmd = argv[1];
    if (o.cmd == "-h" || o.cmd == "--help" || o.cmd == "help") { o.cmd = "help"; return true; }

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* f) -> const char* {
            if (i + 1 >= argc) { err = std::string(f) + " needs a value"; return nullptr; }
            return argv[++i];
        };
        if (a == "-i" || a == "--input") {
            const char* v = val("--input"); if (!v) return false;
            o.inputs.emplace_back(v);
        } else if (a == "--format") {
            const char* v = val("--format"); if (!v) return false;
            if (!fmt_from_name(v, o.fmt)) { err = std::string("bad --format ") + v; return false; }
        } else if (a == "--type") {
            const char* v = val("--type"); if (!v) return false;
            o.type = v;
            if (o.type != "lrit" && o.type != "hrit" && o.type != "ggak" && o.type != "auto") {
                err = "bad --type (lrit|hrit|ggak|auto)"; return false;
            }
        } else if (a == "--rate") {
            const char* v = val("--rate"); if (!v) return false;
            if (!need_float("--rate", v, o.rate, err)) return false;
        } else if (a == "--center") {
            const char* v = val("--center"); if (!v) return false;
            if (!need_float("--center", v, o.center, err)) return false;
        } else if (a == "--baud") {
            const char* v = val("--baud"); if (!v) return false;
            if (!need_float("--baud", v, o.baud, err)) return false;
        } else if (a == "--mod") {
            const char* v = val("--mod"); if (!v) return false;
            o.mod = v;
        } else if (a == "--carrier") {
            const char* v = val("--carrier"); if (!v) return false;
            if (std::strcmp(v, "auto") == 0) { o.carrier_auto = true; }
            else { o.carrier_auto = false; if (!need_float("--carrier", v, o.carrier, err)) return false; }
        } else if (a == "--invert") {
            o.invert = true;
        } else if (a == "--polarity") {
            const char* v = val("--polarity"); if (!v) return false;
            o.polarity = v;
        } else if (a == "--sps") {
            const char* v = val("--sps"); if (!v) return false; o.sps = std::atoi(v);
        } else if (a == "--rrc-alpha") {
            const char* v = val("--rrc-alpha"); if (!v) return false;
            if (!need_float("--rrc-alpha", v, o.rrc_alpha, err)) return false;
        } else if (a == "--rrc-span") {
            const char* v = val("--rrc-span"); if (!v) return false; o.rrc_span = std::atoi(v);
        } else if (a == "--pll-bw") {
            const char* v = val("--pll-bw"); if (!v) return false;
            // 0 means no carrier loop, hold the acquisition phase (GobDump's UI does the same); positive values set the bandwidth in Hz/Rs.
            if (!need_float("--pll-bw", v, o.pll_bw, err)) return false;
            o.pll_bw_set = true;
        } else if (a == "--pll-bw-acq") {
            const char* v = val("--pll-bw-acq"); if (!v) return false;
            if (!need_float("--pll-bw-acq", v, o.pll_bw_acq, err)) return false;
            o.pll_bw_acq_set = true;
        } else if (a == "--clock-bw") {
            const char* v = val("--clock-bw"); if (!v) return false;
            if (!need_float("--clock-bw", v, o.clock_bw, err)) return false;
        } else if (a == "--agc") {
            const char* v = val("--agc"); if (!v) return false; o.agc = v;
        } else if (a == "--agc-ref") {
            const char* v = val("--agc-ref"); if (!v) return false;
            if (!need_float("--agc-ref", v, o.agc_ref, err)) return false;
        } else if (a == "--dc") {
            const char* v = val("--dc"); if (!v) return false; o.dc = v;
        } else if (a == "--iq-balance") {
            const char* v = val("--iq-balance"); if (!v) return false; o.iqbal = v;
        } else if (a == "--skip") {
            const char* v = val("--skip"); if (!v) return false;
            if (!need_float("--skip", v, o.time_offset, err)) return false;
        } else if (a == "--cadu-bits") {
            const char* v = val("--cadu-bits"); if (!v) return false; o.cadu_bits = std::atoi(v);
        } else if (a == "--rs-depth") {
            const char* v = val("--rs-depth"); if (!v) return false; o.rs_depth = std::atoi(v);
        } else if (a == "--derand") {
            const char* v = val("--derand"); if (!v) return false;
            o.derand = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--erasure") {
            const char* v = val("--erasure"); if (!v) return false;
            o.erasure = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--mv") {
            const char* v = val("--mv"); if (!v) return false;
            o.mv = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--dd-refine") {
            const char* v = val("--dd-refine"); if (!v) return false;
            o.dd_refine = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--sova") {
            const char* v = val("--sova"); if (!v) return false;
            o.sova = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--threads") {
            const char* v = val("--threads"); if (!v) return false; o.threads = std::atoi(v);
        } else if (a == "-o" || a == "--out") {
            const char* v = val("--out"); if (!v) return false; o.out = v;
        } else if (a == "--images") {
            const char* v = val("--images"); if (!v) return false;
            o.images = std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
        } else if (a == "--png-depth") {
            const char* v = val("--png-depth"); if (!v) return false; o.png_depth = v;
        } else if (a == "--dump-soft") {
            const char* v = val("--dump-soft"); if (!v) return false; o.dump_soft = v;
        } else if (a == "--dump-bits") {
            const char* v = val("--dump-bits"); if (!v) return false; o.dump_bits = v;
        } else if (a == "--dump-cadu") {
            const char* v = val("--dump-cadu"); if (!v) return false; o.dump_cadu = v;
        } else if (a == "--stats") {
            const char* v = val("--stats"); if (!v) return false; o.stats = v;
        } else if (a == "--json") {
            o.json = true; o.json_stdout = true;
        } else if (a == "-q" || a == "--quiet") {
            o.quiet = true;
        } else if (a == "-v" || a == "--verbose") {
            o.verbose = true;
        } else if (a == "--progress") {
            o.progress = true;
        } else if (a == "--max-seconds") {
            const char* v = val("--max-seconds"); if (!v) return false;
            if (!need_float("--max-seconds", v, o.max_seconds, err)) return false;
        } else if (a == "--stop-after") {
            const char* v = val("--stop-after"); if (!v) return false; o.stop_after = v;
        } else if (a == "--gui") {
            o.gui = true;
        } else if (a == "--gui-frames") {
            const char* v = val("--gui-frames"); if (!v) return false; o.gui_frames = std::atoi(v);
        } else if (a == "--gui-out") {
            const char* v = val("--gui-out"); if (!v) return false; o.gui_out = v;
        } else if (a == "--gui-scale") {
            const char* v = val("--gui-scale"); if (!v) return false;
            if (!need_float("--gui-scale", v, o.gui_scale, err)) return false;
        } else if (a == "--seconds") {
            const char* v = val("--seconds"); if (!v) return false;
            if (!need_float("--seconds", v, o.synth_seconds, err)) return false;
        } else if (a == "--snr") {
            const char* v = val("--snr"); if (!v) return false;
            if (!need_float("--snr", v, o.synth_snr, err)) return false;
        } else if (a == "--offset") {
            const char* v = val("--offset"); if (!v) return false;
            if (!need_float("--offset", v, o.synth_offset, err)) return false;
        } else if (a == "--ppm") {
            const char* v = val("--ppm"); if (!v) return false;
            if (!need_float("--ppm", v, o.synth_ppm, err)) return false;
        } else if (a == "--iq-gain") {
            const char* v = val("--iq-gain"); if (!v) return false;
            if (!need_float("--iq-gain", v, o.synth_iq_gain_db, err)) return false;
        } else if (a == "--iq-phase") {
            const char* v = val("--iq-phase"); if (!v) return false;
            if (!need_float("--iq-phase", v, o.synth_iq_phase_deg, err)) return false;
        } else if (a == "--seed") {
            const char* v = val("--seed"); if (!v) return false; o.synth_seed = (unsigned)std::atoi(v);
        } else if (a == "--full-chain") {
            o.synth_full_chain = true;
        } else if (a == "--pattern") {
            const char* v = val("--pattern"); if (!v) return false; o.synth_pattern = v;
        } else if (a == "--manifest") {
            const char* v = val("--manifest"); if (!v) return false; o.manifest = v;
        } else if (a == "--realtime") {
            const char* v = val("--realtime"); if (!v) return false;
            o.max_seconds = o.max_seconds;  // accepted, handled by bench
        } else {
            err = "unknown option: " + a;
            return false;
        }
    }
    return true;
}

void print_help(const char* argv0) {
    std::printf(
        "elektro-ultra - Elektro-L LRIT/HRIT demodulator & decoder\n"
        "\n"
        "  %s decode  -i FILE [options]     demodulate, FEC, xRIT products\n"
        "  %s probe   -i FILE               report what is in a recording\n"
        "  %s synth   [options]             write a synthetic IQ file\n"
        "  %s selftest [--manifest FILE]    FEC vectors and corpus replay\n"
        "  %s bench   -i FILE               throughput measurement\n"
        "\n"
        "Key options (see PLAN.md for all of them):\n"
        "  --type lrit|hrit|ggak|auto   transmission type (sets baud, mod, presets)\n"
        "  --format cs16|cs8|cu8|cf32|ziq|auto\n"
        "  --rate HZ --center HZ        recording sample rate and RF centre\n"
        "  --baud HZ --mod bpsk|oqpsk|qpsk|auto\n"
        "  --carrier HZ|auto  --invert  --polarity auto|normal|inverted\n"
        "  --sps N --rrc-alpha F --pll-bw F --clock-bw F\n"
        "  --agc on|off|auto --dc on|off|auto --iq-balance on|off|auto\n"
        "  --erasure on|off --mv on|off --dd-refine on|off --sova on|off\n"
        "  -o DIR --images on|off --stats FILE --dump-soft FILE --dump-cadu DIR\n"
        "  --gui [--gui-frames N --gui-out PREFIX.png]\n"
        "  --max-seconds S --stop-after symbols|cadus|frames|products\n"
        "\n"
        "Examples:\n"
        "  %s probe -i 2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16\n"
        "  %s decode -i cap.cs16 --type lrit -o out/lrit\n"
        "  %s decode -i cap.cs16 --type hrit --gui\n"
        "  %s synth --mod bpsk --baud 294000 --rate 3000000 --snr 4 -i /tmp/t.cs16\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

}  // namespace eu
