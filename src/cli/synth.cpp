#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "cli/options.h"
#include "core/common.h"
#include "dsp/modulate.h"
#include "fec/randomiser.h"
#include "fec/rs.h"
#include "fec/viterbi.h"

namespace eu {

static void write_file(const std::string& path, const std::vector<cf>& x, SampleFormat fmt) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { EU_LOGE("cannot write %s", path.c_str()); return; }
    if (fmt == SampleFormat::Cf32) {
        std::fwrite(x.data(), sizeof(cf), x.size(), f);
    } else if (fmt == SampleFormat::Cs8) {
        for (const auto& v : x) {
            int8_t p[2] = {(int8_t)std::lrint(clampf(v.real(), -1.f, 1.f) * 127.0f),
                           (int8_t)std::lrint(clampf(v.imag(), -1.f, 1.f) * 127.0f)};
            std::fwrite(p, 1, 2, f);
        }
    } else {  // cs16
        for (const auto& v : x) {
            int16_t p[2] = {(int16_t)std::lrint(clampf(v.real(), -1.f, 1.f) * 32767.0f),
                            (int16_t)std::lrint(clampf(v.imag(), -1.f, 1.f) * 32767.0f)};
            std::fwrite(p, 2, 2, f);
        }
    }
    std::fclose(f);
    std::printf("wrote %s (%.2f Msamples, %s)\n", path.c_str(), x.size() / 1e6,
                fmt_name(fmt));
}

int run_synth(const Options& o) {
    if (o.inputs.empty()) {
        std::fprintf(stderr, "synth: need an output path with -i FILE.cs16\n");
        return 4;
    }
    const std::string path = o.inputs.front();
    ModConfig cfg;
    cfg.mod = (o.mod == "auto") ? "bpsk" : o.mod;
    cfg.baud = (o.baud > 0) ? o.baud : kLritBaud;
    cfg.rate = (o.rate > 0) ? o.rate : 3000000.0;
    cfg.rrc_alpha = o.rrc_alpha;
    cfg.rrc_span = o.rrc_span;
    cfg.snr_db = o.synth_snr;
    cfg.offset_hz = o.synth_offset;
    cfg.ppm = o.synth_ppm;
    cfg.iq_gain_db = o.synth_iq_gain_db;
    cfg.iq_phase_deg = o.synth_iq_phase_deg;
    cfg.seed = o.synth_seed;

    const int bps = bits_per_symbol(cfg.mod);
    const size_t nsym = (size_t)(cfg.baud * o.synth_seconds);
    std::vector<uint8_t> bits;
    if (o.synth_full_chain) {
        // Real CADUs, so a stubborn Viterbi BER can be told from a bad capture.
        ReedSolomon rs;
        rs.set_dual_basis(true);
        Randomiser rnd;
        std::mt19937 rng(cfg.seed);
        const size_t cadu_bits = 8192;
        const size_t chan_per_cadu = cadu_bits * 2;   // rate 1/2
        const size_t ncadu = (nsym * (size_t)bps) / chan_per_cadu;
        bits.reserve(ncadu * chan_per_cadu);
        for (size_t c = 0; c < ncadu; ++c) {
            std::vector<uint8_t> cadu(1024, 0);
            cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
            for (size_t k = 4; k < 1024; ++k) cadu[k] = (uint8_t)(rng() & 0xFFu);
            for (int pos = 0; pos < 4; ++pos) {
                uint8_t blk[255];
                for (int i = 0; i < 255; ++i) blk[i] = cadu[4 + (size_t)i * 4 + (size_t)pos];
                rs.encode_block(blk);
                for (int i = 0; i < 255; ++i) cadu[4 + (size_t)i * 4 + (size_t)pos] = blk[i];
            }
            rnd.apply(cadu.data() + 4, 1020, 0);
            std::vector<uint8_t> raw;
            raw.reserve(cadu_bits);
            for (size_t k = 0; k < 1024; ++k)
                for (int b = 7; b >= 0; --b) raw.push_back((uint8_t)((cadu[k] >> b) & 1u));
            std::vector<uint8_t> enc;
            Viterbi::encode(raw.data(), raw.size(), 0171, 0133, enc);
            bits.insert(bits.end(), enc.begin(), enc.end());
        }
        bits.resize(ncadu * chan_per_cadu);
        std::printf("synth: %zu real CADUs (ASM + RS + randomiser + conv)\n", ncadu);
    } else {
        bits = random_bits(nsym, bps, cfg.seed);
    }

    std::vector<cf> x;
    const size_t n = modulate(bits, cfg, x);
    const double sps = cfg.rate / cfg.baud;
    add_awgn(x, cfg.snr_db, sps, 1.0 + cfg.rrc_alpha, cfg.seed ^ 0x9e3779b9u);

    SampleFormat out_fmt = fmt_from_path(path);
    if (out_fmt != SampleFormat::Cs8 && out_fmt != SampleFormat::Cf32) out_fmt = SampleFormat::Cs16;
    std::printf("synth: %s %.0f Bd, rate %.0f, sps %.2f, in-band snr %.1f dB, offset %+.0f Hz, "
                "%zu symbols\n",
                cfg.mod.c_str(), cfg.baud, cfg.rate, sps, cfg.snr_db, cfg.offset_hz, n);
    write_file(path, x, out_fmt);
    return 0;
}


int run_noisify(const Options& o) {
    if (o.inputs.empty() || o.out.empty()) {
        std::fprintf(stderr, "noisify: need -i clean.cs16 -o noisy.cs16\n");
        return 4;
    }
    const std::string in_path = o.inputs[0];
    const std::string out_path = o.out;
    FILE* fi = std::fopen(in_path.c_str(), "rb");
    if (!fi) { std::fprintf(stderr, "noisify: cannot read %s\n", in_path.c_str()); return 4; }
    FILE* fo = std::fopen(out_path.c_str(), "wb");
    if (!fo) { std::fclose(fi); std::fprintf(stderr, "noisify: cannot write %s\n", out_path.c_str()); return 4; }

    const double fs = o.rate > 0 ? o.rate : 4600000.0;
    const double baud = o.baud > 0 ? o.baud : kHritBaud;
    const double alpha = o.rrc_alpha > 0 ? o.rrc_alpha : 0.5;
    double occ = (1.0 + alpha) * baud / fs;
    occ = occ > 1.0 ? 1.0 : occ;

    const size_t chunk = 1u << 20;
    std::vector<int16_t> buf(chunk * 2);
    std::mt19937 rng(o.synth_seed);
    std::normal_distribution<double> g(0.0, 1.0);

    // Power from the file itself, not a constant that fits one recording.
    const size_t got = std::fread(buf.data(), sizeof(int16_t), buf.size(), fi);
    double p = 0.0;
    for (size_t i = 0; i + 1 < got; i += 2) {
        const double re = buf[i] / 32768.0, im = buf[i + 1] / 32768.0;
        p += re * re + im * im;
    }
    const size_t n_s = got / 2;
    p = n_s ? p / (double)n_s : 1e-6;

    // sigma^2 = P*(fs/Rs)/(Es/N0); the factor wrong makes a sweep that measures nothing.
    const double snr = std::pow(10.0, o.synth_snr / 10.0);
    const double var = p * (fs / baud) / snr;  // complex noise power
    const double sigma = std::sqrt(var * 0.5);

    std::fseek(fi, 0, SEEK_SET);
    uint64_t written = 0;
    size_t readn = 0;
    while ((readn = std::fread(buf.data(), sizeof(int16_t), buf.size(), fi)) > 0) {
        for (size_t i = 0; i + 1 < readn; i += 2) {
            double re = buf[i] / 32768.0 + sigma * g(rng);
            double im = buf[i + 1] / 32768.0 + sigma * g(rng);
            re = re > 1.0 ? 1.0 : (re < -1.0 ? -1.0 : re);
            im = im > 1.0 ? 1.0 : (im < -1.0 ? -1.0 : im);
            int16_t out[2] = {(int16_t)std::lrint(re * 32767.0), (int16_t)std::lrint(im * 32767.0)};
            std::fwrite(out, sizeof(int16_t), 2, fo);
        }
        written += readn / 2;
    }
    std::fclose(fi);
    std::fclose(fo);
    std::printf("noisify: %s -> %s, %.2f Msamples, signal p %.3e, occupied %.1f%%, "
                "added Es/N0 %.1f dB, sigma %.4f\n",
                in_path.c_str(), out_path.c_str(), written / 1e6, p, occ * 100.0,
                o.synth_snr, sigma);
    return 0;
}

}  // namespace eu
