// FEC-only ladder: ideal soft bits plus calibrated noise.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <random>
#include <vector>
#include "fec/randomiser.h"
#include "fec/viterbi.h"
#include "fec/fec_chain.h"
using namespace eu;

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: fecprobe <ref.cadu> <start_frame> <n_frames> [esn0...]\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, (long)std::atoi(argv[2]) * 1024, SEEK_SET);
    const int nframes = std::atoi(argv[3]);
    Randomiser rnd;
    std::vector<uint8_t> rec(1024), bits;
    for (int k = 0; k < nframes; ++k) {
        if (std::fread(rec.data(), 1, 1024, f) != 1024) break;
        std::vector<uint8_t> body(rec.begin() + 4, rec.end());
        rnd.apply(body.data(), body.size(), 0);
        for (int i = 0; i < 4; ++i)
            for (int b = 7; b >= 0; --b) bits.push_back((rec[i] >> b) & 1);
        for (uint8_t v : body)
            for (int b = 7; b >= 0; --b) bits.push_back((v >> b) & 1);
    }
    std::fclose(f);
    std::vector<uint8_t> chan;
    Viterbi::encode(bits.data(), bits.size(), 0171, 0133, chan);
    std::printf("%d frames, %zu payload bits, %zu channel bits\n", nframes, bits.size(), chan.size());

    for (int a = 4; a < argc; ++a) {
        const double esn0_db = std::atof(argv[a]);
        const double esn0 = std::pow(10.0, esn0_db / 10.0);
        const double eb = esn0 / 2.0;
        const double sigma = std::sqrt(1.0 / eb);
        std::mt19937 rng(11);
        std::normal_distribution<double> g(0.0, sigma);
        std::vector<int8_t> soft(chan.size());
        for (size_t i = 0; i < chan.size(); ++i) {
            double v = (chan[i] ? 1.0 : -1.0) + g(rng);
            long q = std::lround(v * 40.0);
            soft[i] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
        }
        FecChain fec;
        fec.configure(8192, 4, true, true, true, 4);
        std::vector<std::vector<uint8_t>> out;
        fec.push(soft.data(), soft.size(), out);
        const FecStats& st = fec.stats();
        std::printf("Es/N0 %4.1f dB -> cadus %llu, good %llu, rs fixed %llu, failed %llu, ber %.4f\n",
                    esn0_db, (unsigned long long)st.cadus, (unsigned long long)st.cadus_good,
                    (unsigned long long)st.rs_corrected, (unsigned long long)st.rs_failed, st.viterbi_ber);
    }
    return 0;
}
