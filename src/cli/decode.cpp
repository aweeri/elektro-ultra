// IQ -> demod -> FEC -> frames -> products.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cli/options.h"
#include "core/analysis.h"
#include "core/common.h"
#include "core/io.h"
#include "gui/app.h"
#include "dsp/demod.h"
#include "fec/fec_chain.h"
#include "xrit/products.h"
#include "xrit/transport.h"

namespace eu {

namespace {

struct RunResult {
    uint64_t samples = 0;
    uint64_t soft = 0;
    uint64_t payloads = 0;
    double   seconds = 0.0;
};

std::string human(double v, const char* unit) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f %s", v, unit);
    return b;
}

}  // namespace

int run_decode(const Options& o) {
    if (o.gui) return run_gui(o);
    if (o.inputs.empty()) {
        std::fprintf(stderr, "decode: need -i FILE\n");
        return 4;
    }
    const std::string path = o.inputs.front();

    std::string err;
    auto src = SampleSource::open(path, o.fmt, o.rate, o.center, &err);
    if (!src) {
        std::fprintf(stderr, "decode: %s\n", err.c_str());
        return 4;
    }
    const double fs = src->info().sample_rate > 0 ? src->info().sample_rate : o.rate;
    if (fs <= 0) {
        std::fprintf(stderr, "decode: sample rate unknown; pass --rate\n");
        return 4;
    }

    const size_t kBlock = 1u << 17;          // 131072 samples
    std::vector<cf> buf(kBlock);
    size_t got = src->read(buf.data(), kBlock);
    if (got < 16384) {
        std::fprintf(stderr, "decode: file too short (%zu samples)\n", got);
        return 4;
    }

    std::string type = o.type;
    double baud = o.baud;
    Mod mod = Mod::Bpsk;
    bool have_mod = false;

    if (o.mod != "auto") {
        if (!mod_from_name(o.mod, mod)) {
            std::fprintf(stderr, "decode: bad --mod %s\n", o.mod.c_str());
            return 4;
        }
        have_mod = true;
    }

    ProbeResult pr;
    if (type == "auto" || baud <= 0 || !have_mod) {
        pr = probe_samples(buf.data(), got, fs, baud);
        if (type == "auto") {
            if (pr.rate_found) {
                const double r = pr.symbol_rate;
                if (std::fabs(r - kLritBaud) < 0.02 * kLritBaud) { type = "lrit"; baud = kLritBaud; }
                else if (std::fabs(r - kHritBaud) < 0.02 * kHritBaud) { type = "hrit"; baud = kHritBaud; }
                else { type = (r > 500000.0) ? "hrit" : "lrit"; baud = r; }
            } else {
                type = "lrit";
                baud = kLritBaud;
            }
            EU_LOGI("auto: %s at %.0f Bd (%s, line %.1f dB)", type.c_str(), baud,
                    pr.mod_guess.c_str(), pr.rate_strength_db);
        }
        if (baud <= 0) baud = (type == "hrit") ? kHritBaud : kLritBaud;
        if (!have_mod) {
            if (type == "hrit") mod = Mod::Oqpsk;
            else if (pr.mod_guess == "qpsk/oqpsk") mod = Mod::Oqpsk;
            else mod = Mod::Bpsk;
        }
    }
    if (baud <= 0) baud = (type == "hrit") ? kHritBaud : kLritBaud;

    DemodConfig dc;
    dc.mod = mod;
    dc.baud = baud;
    dc.sps = std::max(2, std::min(16, o.sps));
    dc.rrc_alpha = o.rrc_alpha > 0 ? o.rrc_alpha : 0.5;
    // In Hz, not normalised: 2500 Hz cost 0.4 dB of MER and moved the HRIT threshold from 3.9 to 4.5 dB Es/N0.
    dc.clock_bw = o.clock_bw > 0 ? o.clock_bw
                                : std::min(0.02, std::max(0.0002, 1000.0 / baud));
    dc.pll_bw = o.pll_bw_set ? o.pll_bw : 0.0015;
    dc.pll_bw_acq = o.pll_bw_acq_set ? o.pll_bw_acq : 0.003;
    dc.dc_block = (o.dc != "off");
    dc.agc = (o.agc != "off");
    dc.invert = o.invert;
    dc.oqpsk_align = true;
    dc.carrier_auto = o.carrier_auto;
    dc.carrier_hz = o.carrier;
    dc.search_hz = std::max(5000.0, std::min(200000.0, baud * 0.5));

    DemodChain demod;
    if (!demod.configure(dc, fs, &err)) {
        std::fprintf(stderr, "decode: %s\n", err.c_str());
        return 4;
    }

    FecChain fec;
    fec.configure(o.cadu_bits, o.rs_depth, o.derand, true, o.erasure,
                  dc.mod == Mod::Bpsk ? 2 : 4);

    TransportDemux transport;
    ProductWriter products;
    ProductStats pstats;
    std::vector<XritBlob> blobs;
    std::string out_root = o.out;
    if (out_root.empty()) out_root = "out/" + capture_stem(path);
    products.configure(out_root, type, o.images);

    if (type == "ggak")
        EU_LOGI("ggak: demodulating only, product output is disabled by design");

    FILE* soft_dump = nullptr;
    if (!o.dump_soft.empty()) soft_dump = std::fopen(o.dump_soft.c_str(), "wb");
    FILE* cadu_dump = nullptr;
    if (!o.dump_cadu.empty()) {
        cadu_dump = std::fopen(o.dump_cadu.c_str(), "wb");
        if (!cadu_dump) {
            std::fprintf(stderr, "decode: cannot write %s\n", o.dump_cadu.c_str());
            return 4;
        }
    }

    std::vector<int8_t> soft;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<cf> probe_buf;
    const size_t kProbeMax = 1u << 21;
    RunResult rr;
    Timer total;
    Timer tick;
    const double max_s = o.max_seconds > 0 ? o.max_seconds : 0.0;

    double t_demod = 0.0, t_fec = 0.0;
    auto feed = [&](const cf* x, size_t n) {
        // Head of the capture, for a symbol-rate estimate that does not inherit the Gardner S-curve bias.
        if (probe_buf.size() < kProbeMax) {
            const size_t take = std::min(n, kProbeMax - probe_buf.size());
            probe_buf.insert(probe_buf.end(), x, x + take);
        }
        soft.clear();
        const auto t0 = std::chrono::steady_clock::now();
        demod.process(x, n, soft);
        t_demod += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto t1 = std::chrono::steady_clock::now();
        if (!soft.empty()) {
            rr.soft += soft.size();
            if (soft_dump) std::fwrite(soft.data(), 1, soft.size(), soft_dump);
            payloads.clear();
            fec.push(soft.data(), soft.size(), payloads);
            for (const auto& p : payloads) {
                ++rr.payloads;
                blobs.clear();
                transport.push(p.data(), p.size(), blobs);
                for (const auto& b : blobs) products.push(b, pstats);
                if (cadu_dump) {
                    const uint8_t len[2] = {(uint8_t)(p.size() & 0xFF),
                                            (uint8_t)((p.size() >> 8) & 0xFF)};
                    std::fwrite(len, 1, 2, cadu_dump);
                    std::fwrite(p.data(), 1, p.size(), cadu_dump);
                }
            }
        }
        t_fec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    };

    feed(buf.data(), got);
    rr.samples += got;

    while (true) {
        got = src->read(buf.data(), kBlock);
        if (got == 0) break;
        rr.samples += got;
        feed(buf.data(), got);

        if (max_s > 0 && (double)rr.samples / fs >= max_s) break;
        if (o.progress && tick.ms() > 1000.0) {
            tick.reset();
            const FecStats& f = fec.stats();
            const DemodStats& d = demod.stats();
            std::fprintf(stderr,
                         "\r  %.1fs  snr %5.1f dB  ber %.3f  cadu %llu/%llu%s",
                         (double)rr.samples / fs, d.snr_db, f.viterbi_ber,
                         (unsigned long long)f.cadus_good, (unsigned long long)f.cadus,
                         f.deframer_locked ? "  SYNC" : "      ");
        }
    }
    if (o.progress) std::fprintf(stderr, "\n");

    soft.clear();
    demod.flush(soft);
    if (!soft.empty()) {
        payloads.clear();
        fec.push(soft.data(), soft.size(), payloads);
        rr.payloads += payloads.size();
    }
    if (soft_dump) std::fclose(soft_dump);
    products.flush(pstats);
    if (cadu_dump) std::fclose(cadu_dump);

    const DemodStats& d = demod.stats();
    const FecStats& f = fec.stats();
    const double secs = (double)rr.samples / fs;
    const double cadu_rate = secs > 0 ? (double)f.cadus / secs : 0.0;
    const double expect = (type == "hrit") ? kHritCaduRate : kLritCaduRate;

    double baud_line_db = 0.0;
    const double baud_est = probe_buf.size() >= (1u << 17)
                                ? estimate_symbol_rate(probe_buf.data(), probe_buf.size(), fs,
                                                       baud_line_db, baud * 0.5, baud * 1.5)
                                : 0.0;
    const double ppm = baud_est > 0.0 ? (baud_est / baud - 1.0) * 1e6 : 0.0;

    std::printf("file       : %s\n", path.c_str());
    std::printf("type       : %s  %s  %.0f Bd  %.3f MSPS\n", type.c_str(),
                mod_name(dc.mod), baud, fs / 1e6);
    std::printf("demod      : mer %.1f dB  evm %.1f%%  m2m4 %.1f dB  carrier %+.1f Hz  clock %+.1f ppm\n",
                d.snr_db, d.evm_pct, d.m2m4_db, d.carrier_hz, ppm);
    std::printf("baud       : measured %.0f Bd vs nominal %.0f Bd (line %.1f dB)\n",
                baud_est, baud, baud_line_db);
    std::printf("lock       : viterbi %s (ber %.3f)  deframer %s (asm %.2f)\n",
                f.viterbi_locked ? "locked" : "-", f.viterbi_ber,
                f.deframer_locked ? "locked" : "-", f.asm_confidence);
    std::printf("frames     : %.1f/s  good %llu/%llu  rs fixed %llu  failed %llu\n",
                cadu_rate, (unsigned long long)f.cadus_good,
                (unsigned long long)f.cadus, (unsigned long long)f.rs_corrected,
                (unsigned long long)f.rs_failed);
    if (f.cadus_good > 0)
        std::printf("rate       : implied %.0f Bd vs nominal %.0f Bd (%+.1f%%, file average)\n",
                    cadu_rate * 16384.0, (type == "hrit") ? kHritBaud : kLritBaud,
                    expect > 0 ? (cadu_rate / expect - 1.0) * 100.0 : 0.0);
    else
        std::printf("rate       : no frame decoded, rate readout is meaningless\n");
    std::printf("transport  : %llu packets (%llu bad crc)  %llu xRIT files, %llu complete\n",
                (unsigned long long)transport.packets(),
                (unsigned long long)transport.crc_bad(),
                (unsigned long long)pstats.files_seen,
                (unsigned long long)pstats.files_complete);
    std::printf("products   : %llu images -> %llu png, %llu stitched, %llu raw, %llu unsupported  (%s)\n",
                (unsigned long long)pstats.images, (unsigned long long)pstats.pngs,
                (unsigned long long)pstats.stitched, (unsigned long long)pstats.raws,
                (unsigned long long)pstats.unsupported, out_root.c_str());
    if (std::getenv("EU_PROFILE")) {
        static const char* names[6] = {"dc", "nco", "resample", "rrc", "rail", "agc"};
        const double* t = demod.stage_times();
        double sum = 0.0;
        for (int i = 0; i < 6; ++i) sum += t[i];
        std::printf("profile    : ");
        for (int i = 0; i < 6; ++i)
            std::printf("%s %.2fs (%.0f%%)  ", names[i], t[i],
                        sum > 0 ? 100.0 * t[i] / sum : 0.0);
        std::printf("total %.2fs\n", sum);
        std::printf("split      : demod %.2fs  fec+transport %.2fs  (rest: io/stats)\n", t_demod, t_fec);
    }
    std::printf("throughput : %.1fs of IQ in %.1fs (%.1fx realtime)\n", secs,
                total.ms() / 1000.0, total.ms() > 0 ? secs / (total.ms() / 1000.0) : 0.0);

    if (f.cadus == 0) return 2;                    // never locked
    if (f.cadus_good < f.cadus) return 3;          // partial
    return 0;
}

}  // namespace eu
