// Demodulator self-test: synthesise a known BPSK/OQPSK stream, demodulate, and measure BER against the transmitted bits.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/common.h"
#include "dsp/demod.h"
#include "dsp/modulate.h"

using namespace eu;

static int ber_for_shift(const std::vector<int8_t>& soft,
                         const std::vector<uint8_t>& bits, int bits_per_sym,
                         int shift) {
    int bad = 0, n = 0;
    for (size_t i = 0; i + (size_t)shift < bits.size(); ++i) {
        const int s = soft[i * bits_per_sym] > 0 ? 1 : 0;
        if (s != (int)(bits[i + (size_t)shift] & 1)) ++bad;
        ++n;
        if (n > 20000) break;
    }
    return bad;
}

int main(int argc, char** argv) {
    const char* modname = argc > 1 ? argv[1] : "bpsk";
    const double snr = argc > 2 ? std::atof(argv[2]) : 40.0;
    const double offset = argc > 3 ? std::atof(argv[3]) : 0.0;

    ModConfig mc;
    mc.mod = modname;
    mc.baud = (std::strcmp(modname, "bpsk") == 0) ? kLritBaud : kHritBaud;
    mc.rate = (std::strcmp(modname, "bpsk") == 0) ? 3e6 : 6e6;
    mc.snr_db = snr;
    mc.offset_hz = offset;
    mc.seed = 7;

    const int bps = bits_per_symbol(mc.mod);
    const size_t nsym = (size_t)(mc.baud * 0.4);
    std::vector<uint8_t> bits = random_bits(nsym, bps, mc.seed);

    std::vector<cf> x;
    modulate(bits, mc, x);
    add_awgn(x, mc.snr_db, mc.rate / mc.baud, 1.0 + mc.rrc_alpha, 12345u);

    DemodConfig dc;
    dc.mod = (bps == 1) ? Mod::Bpsk : Mod::Oqpsk;
    dc.baud = mc.baud;
    dc.sps = 4;

    DemodChain chain;
    std::string err;
    if (!chain.configure(dc, mc.rate, &err)) {
        std::printf("configure failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<int8_t> soft;
    chain.process(x.data(), x.size(), soft);
    chain.flush(soft);

    const DemodStats& st = chain.stats();
    std::printf("%s snr_in %.1f offset %.0f -> symbols %llu, soft %zu, evm %.1f%%, "
                "snr_est %.1f dB, carrier %.1f Hz\n",
                modname, snr, offset, (unsigned long long)st.symbols, soft.size(),
                st.evm_pct, st.snr_db, st.carrier_hz);

    // Best lag alignment, plus the constellation spread for a quick eyeball.
    int best = 1 << 30, best_shift = 0;
    for (int sh = 0; sh < 200; ++sh) {
        const int b = ber_for_shift(soft, bits, bps, sh);
        if (b < best) { best = b; best_shift = sh; }
    }
    std::printf("best BER %.4f at lag %d (%d/%d wrong in first 20000)\n",
                best / 20000.0, best_shift, best, 20000);

    const auto& c = chain.constellation();
    double mx = 0;
    for (const auto& v : c) mx = std::max(mx, (double)std::abs(v));
    std::printf("constellation: %zu pts, peak |y| %.3f\n", c.size(), mx);
    for (size_t i = 0; i < std::min<size_t>(8, c.size()); ++i)
        std::printf("  [%zu] %+.3f %+.3fj\n", i, c[i].real(), c[i].imag());
    return best < 20000 / 100;
}
