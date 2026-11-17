// Channel BER against the known transmitted bits.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "fec/randomiser.h"
#include "fec/viterbi.h"
using namespace eu;

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: chanber <ref.cadu> <soft.bin> <start_frame>\n"); return 2; }
    const int start = std::atoi(argv[3]);
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    if (std::fseek(f, (long)start * 1024, SEEK_SET) != 0) return 2;
    Randomiser rnd;
    std::vector<uint8_t> rec(1024), bits, expect;
    while (std::fread(rec.data(), 1, 1024, f) == 1024) {
        std::vector<uint8_t> body(rec.begin() + 4, rec.end());
        rnd.apply(body.data(), body.size(), 0);
        for (int i = 0; i < 4; ++i)
            for (int b = 7; b >= 0; --b) bits.push_back((rec[i] >> b) & 1);
        for (uint8_t v : body)
            for (int b = 7; b >= 0; --b) bits.push_back((v >> b) & 1);
    }
    std::fclose(f);
    Viterbi::encode(bits.data(), bits.size(), 0171, 0133, expect);
    std::printf("expected %zu channel bits from frame %d (%zu payload bits)\n",
                expect.size(), start, bits.size());

    FILE* g = std::fopen(argv[2], "rb");
    if (!g) { std::printf("cannot open %s\n", argv[2]); return 2; }
    std::vector<int8_t> soft;
    {
        std::vector<char> buf(1 << 20);
        size_t r;
        while ((r = std::fread(buf.data(), 1, buf.size(), g)) > 0) {
            const size_t at = soft.size();
            soft.resize(at + r);
            std::memcpy(soft.data() + at, buf.data(), r);
        }
    }
    std::fclose(g);
    std::printf("soft %zu values\n", soft.size());

    const size_t win = 20000;
    const size_t hint = (argc > 4) ? (size_t)std::atoll(argv[4]) : 0;
    const size_t lo = hint > 8000 ? hint - 8000 : 0;
    const size_t hi = hint + 8000;
    size_t best_off = lo, best_bad = SIZE_MAX;
    int sign = 1;
    for (int sg : {1, -1}) {
        for (size_t off = lo; off < hi && off + win < soft.size() && off + win < expect.size(); ++off) {
            size_t bad = 0;
            for (size_t i = 0; i < win; ++i)
                bad += (((soft[off + i] * sg) > 0) != (expect[i] != 0));
            if (bad < best_bad) { best_bad = bad; best_off = off; sign = sg; }
        }
    }
    std::printf("polarity %+d\n", sign);
    std::printf("alignment offset %zu (%.2f %% on the window)\n", best_off, 100.0 * best_bad / win);

    const size_t n = std::min(soft.size() - best_off, expect.size());
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) bad += (((soft[best_off + i] * sign) > 0) != (expect[i] != 0));
    std::printf("channel BER %.5f over %.2f Mbit\n", (double)bad / n, n / 1e6);
    return 0;
}
