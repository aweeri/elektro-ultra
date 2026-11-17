// selftest / bench.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "cli/options.h"
#include "core/common.h"
#include "dsp/resample.h"
#include "fec/randomiser.h"
#include "fec/rs.h"
#include "fec/viterbi.h"

namespace eu {

namespace {

int failures = 0;

void check(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-34s %s%s%s\n", name, ok ? "ok" : "FAIL",
                detail[0] ? "  " : "", detail);
    if (!ok) ++failures;
}

void test_randomiser() {
    Randomiser r;
    const uint8_t want[8] = {0xFF, 0x48, 0x0E, 0xC0, 0x9A, 0x0D, 0x70, 0xBC};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok = ok && r.table()[i] == want[i];
    char buf[64];
    std::snprintf(buf, sizeof buf, "first byte %02X (want FF)", r.table()[0]);
    check("randomiser CCSDS sequence", ok, buf);
}

void test_rs() {
    ReedSolomon rs(true);
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> d(0, 255);

    uint8_t block[255];
    for (int i = 0; i < 223; ++i) block[i] = (uint8_t)d(rng);
    rs.encode_block(block);

    uint8_t original[255];
    std::memcpy(original, block, 255);

    uint8_t copy[255];
    std::memcpy(copy, block, 255);
    check("RS clean word -> 0 corrections", rs.decode_block(copy) == 0);

    std::memcpy(copy, block, 255);
    bool used[255] = {false};
    for (int i = 0; i < 16; ++i) {
        int pos = (int)(rng() % 255);
        while (used[pos]) pos = (pos + 1) % 255;
        used[pos] = true;
        copy[pos] ^= (uint8_t)(1 + rng() % 255);
    }
    const int fixed = rs.decode_block(copy);
    const bool same = std::memcmp(copy, original, 223) == 0;
    check("RS corrects 16 errors", fixed >= 0 && same);

    std::memcpy(copy, block, 255);
    uint8_t erase[8];
    std::memset(used, 0, sizeof used);
    for (int i = 0; i < 8; ++i) {
        int pos = (int)(rng() % 255);
        while (used[pos]) pos = (pos + 1) % 255;
        used[pos] = true;
        erase[i] = (uint8_t)pos;
        copy[pos] ^= (uint8_t)(1 + rng() % 250);
    }
    for (int i = 0; i < 12; ++i) {
        int pos = (int)(rng() % 255);
        while (used[pos]) pos = (pos + 1) % 255;
        used[pos] = true;
        copy[pos] ^= (uint8_t)(1 + rng() % 250);
    }
    const int e_fixed = rs.decode_block(copy, erase, 8);
    const bool e_same = std::memcmp(copy, original, 223) == 0;
    check("RS erasures 8 + 12 errors", e_fixed >= 0 && e_same);

    std::memcpy(copy, block, 255);
    for (int i = 0; i < 17; ++i) {
        const int pos = (int)(rng() % 255);
        copy[pos] ^= (uint8_t)(1 + rng() % 255);
    }
    const int over = rs.decode_block(copy);
    check("RS refuses 17 errors", over < 0 || std::memcmp(copy, original, 223) == 0);
}

void test_viterbi() {
    std::mt19937 rng(99);
    const size_t nbits = 4096;
    std::vector<uint8_t> bits(nbits);
    for (auto& b : bits) b = (uint8_t)(rng() & 1);

    std::vector<uint8_t> coded;
    Viterbi::encode(bits.data(), bits.size(), 0171, 0133, coded);

    std::vector<int8_t> soft(coded.size());
    for (size_t i = 0; i < coded.size(); ++i) soft[i] = coded[i] ? 100 : -100;

    Viterbi v;
    std::vector<uint8_t> out;
    v.push(soft.data(), soft.size(), out);

    size_t bad = 0;
    const size_t n = std::min(out.size(), bits.size());
    for (size_t i = 64; i < n; ++i) bad += (out[i] != bits[i]);
    const double ber = n > 64 ? (double)bad / (double)(n - 64) : 1.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "ber %.4f over %zu bits", ber, n - 64);
    check("viterbi round trip", ber < 1e-3, buf);
}

void test_fracdelay() {
    FracDelay dl;
    dl.reset(64);
    for (int i = 0; i < 100; ++i) dl.push(cf((float)i, 0));
    bool ok = true;
    for (int t = 0; t <= 20; ++t)
        ok = ok && std::fabs(dl.at((double)t).real() - (99.0 - t)) < 1e-3;
    ok = ok && std::fabs(dl.at(2.5).real() - 96.5) < 0.2;
    check("fractional delay (integer + half)", ok);
}

}  // namespace

int run_selftest(const Options& o) {
    (void)o;
    std::printf("elektro-ultra selftest\n");
    test_randomiser();
    test_rs();
    test_viterbi();
    test_fracdelay();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "all green", failures);
    return failures ? 1 : 0;
}

int run_bench(const Options& o) {
    (void)o;
    std::printf("bench: use decode --max-seconds N and read the throughput line\n");
    return 0;
}

}  // namespace eu
