#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cli/options.h"
#include "core/analysis.h"
#include "core/common.h"
#include "core/io.h"

namespace eu {

int run_probe(const Options& o) {
    if (o.inputs.empty()) {
        std::fprintf(stderr, "probe: need -i FILE\n");
        return 4;
    }
    const std::string path = o.inputs.front();
    std::string err;
    auto src = SampleSource::open(path, o.fmt, o.rate, o.center, &err);
    if (!src) {
        std::fprintf(stderr, "probe: %s\n", err.c_str());
        return 4;
    }
    const SourceInfo& in = src->info();

    const double fs = in.sample_rate > 0 ? in.sample_rate : 0.0;
    std::printf("file      : %s\n", path.c_str());
    std::printf("format    : %s (%d bit)\n", fmt_name(in.fmt), in.bits);
    std::printf("rate      : %.0f Hz (%s)\n", fs, in.rate_source.c_str());
    if (in.center_freq > 0) std::printf("centre    : %.0f Hz\n", in.center_freq);
    if (in.total_samples)
        std::printf("samples   : %llu (%.2f s at %.0f Hz)\n",
                    (unsigned long long)in.total_samples,
                    in.total_samples / (fs > 0 ? fs : 1.0), fs);

    const size_t want = 1u << 19;
    std::vector<cf> buf(want);
    size_t n = 0;
    while (n < want) {
        const size_t got = src->read(buf.data() + n, want - n);
        if (got == 0) break;
        n += got;
    }
    if (n < 4096) {
        std::fprintf(stderr, "probe: only %zu samples readable\n", n);
        return 4;
    }
    if (fs <= 0) {
        std::fprintf(stderr, "probe: sample rate unknown, pass --rate\n");
        return 4;
    }

    double expected = 0;
    if (o.type == "lrit") expected = kLritBaud;
    if (o.type == "hrit") expected = kHritBaud;
    if (o.baud > 0) expected = o.baud;

    const ProbeResult r = probe_samples(buf.data(), n, fs, expected);
    std::printf("level     : rms %.1f dBFS, dc %.1f dB of rms, clip %.4f%%, dc spur %.1f dB\n",
                r.rms_dbfs, db(r.dc_mag), 100.0 * r.clip_frac, r.dc_spur_db);
    std::printf("spectrum  : floor %.1f dBFS/bin, peak %+.1f kHz (+%.1f dB)\n",
                r.noise_floor_dbfs, r.peak_offset_hz / 1e3, r.peak_db);
    if (r.band_found)
        std::printf("band      : %+.1f .. %+.1f kHz (%.1f kHz wide), centroid %+.1f kHz, "
                    "in-band snr %.1f dB\n",
                    r.band_lo_hz / 1e3, r.band_hi_hz / 1e3, r.band_width_hz / 1e3,
                    r.centroid_hz / 1e3, r.inband_snr_db);
    else
        std::printf("band      : not found\n");
    if (r.rate_found)
        std::printf("symbolrate: %.0f Bd (%.1f dB line)\n", r.symbol_rate, r.rate_strength_db);
    else
        std::printf("symbolrate: not found\n");
    std::printf("marks     : x^2 %.1f dB, x^4 %.1f dB -> %s%s\n", r.square_line_db,
                r.fourth_line_db, r.mod_guess.c_str(), r.looks_empty ? "   [NO SIGNAL]" : "");

    const char* type = "auto";
    if (r.rate_found) {
        if (std::fabs(r.symbol_rate - kLritBaud) < 0.02 * kLritBaud) type = "lrit";
        else if (std::fabs(r.symbol_rate - kHritBaud) < 0.02 * kHritBaud) type = "hrit";
    }
    std::printf("suggest   : %s decode -i %s --type %s", "", path.c_str(), type);
    if (r.band_found)
        std::printf(" --carrier %.0f", r.centroid_hz);
    std::printf("\n");
    return r.looks_empty ? 3 : 0;
}

}  // namespace eu
