#include "fec/rs.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace eu {

namespace {

constexpr uint16_t kPrimPoly = 0x187;   // x^8 + x^7 + x^2 + x + 1
constexpr int kOrder = 255;
constexpr int kRoots = 32;
constexpr int kFirstRoot = 212;         // 11*112 mod 255
constexpr int kGap = 11;
constexpr int kN = 255;
constexpr int kK = 223;

struct Gf {
    uint8_t exp[512];
    uint8_t log[256];
    // 256x256 product table: multiplying is the hottest operation in the decoder (32 roots x 255 symbols x 4 codewords per frame).
    std::vector<uint8_t> mul;
    Gf() {
        int x = 1;
        for (int i = 0; i < kOrder; ++i) {
            exp[i] = (uint8_t)x;
            log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= kPrimPoly;
        }
        for (int i = kOrder; i < 512; ++i) exp[i] = exp[i - kOrder];
        mul.assign(65536, 0);
        for (int a = 1; a < 256; ++a)
            for (int b = 1; b < 256; ++b)
                mul[(a << 8) | b] = exp[log[a] + log[b]];
    }
};

const Gf& gf() {
    static const Gf g;
    return g;
}

inline uint8_t gmul(uint8_t a, uint8_t b) {
    return gf().mul[((int)a << 8) | b];
}

inline uint8_t ginv(uint8_t a) {
    const Gf& f = gf();
    return f.exp[(kOrder - f.log[a]) % kOrder];
}

inline uint8_t gpow(uint8_t a, int n) {
    if (!a) return 0;
    const Gf& f = gf();
    int e = (int)f.log[a] * (n % kOrder) % kOrder;
    if (e < 0) e += kOrder;
    return f.exp[e];
}

const std::array<uint8_t, kRoots>& roots() {
    static const std::array<uint8_t, kRoots> r = [] {
        std::array<uint8_t, kRoots> v{};
        for (int j = 0; j < kRoots; ++j) v[j] = gf().exp[(kFirstRoot + kGap * j) % kOrder];
        return v;
    }();
    return r;
}

// CCSDS 131.0-B dual-basis transform.
const uint8_t kFromDual[256] = {
    0x00, 0xcc, 0xac, 0x60, 0x79, 0xb5, 0xd5, 0x19, 0xf0, 0x3c, 0x5c, 0x90, 0x89, 0x45, 0x25, 0xe9,
    0xfd, 0x31, 0x51, 0x9d, 0x84, 0x48, 0x28, 0xe4, 0x0d, 0xc1, 0xa1, 0x6d, 0x74, 0xb8, 0xd8, 0x14,
    0x2e, 0xe2, 0x82, 0x4e, 0x57, 0x9b, 0xfb, 0x37, 0xde, 0x12, 0x72, 0xbe, 0xa7, 0x6b, 0x0b, 0xc7,
    0xd3, 0x1f, 0x7f, 0xb3, 0xaa, 0x66, 0x06, 0xca, 0x23, 0xef, 0x8f, 0x43, 0x5a, 0x96, 0xf6, 0x3a,
    0x42, 0x8e, 0xee, 0x22, 0x3b, 0xf7, 0x97, 0x5b, 0xb2, 0x7e, 0x1e, 0xd2, 0xcb, 0x07, 0x67, 0xab,
    0xbf, 0x73, 0x13, 0xdf, 0xc6, 0x0a, 0x6a, 0xa6, 0x4f, 0x83, 0xe3, 0x2f, 0x36, 0xfa, 0x9a, 0x56,
    0x6c, 0xa0, 0xc0, 0x0c, 0x15, 0xd9, 0xb9, 0x75, 0x9c, 0x50, 0x30, 0xfc, 0xe5, 0x29, 0x49, 0x85,
    0x91, 0x5d, 0x3d, 0xf1, 0xe8, 0x24, 0x44, 0x88, 0x61, 0xad, 0xcd, 0x01, 0x18, 0xd4, 0xb4, 0x78,
    0xc5, 0x09, 0x69, 0xa5, 0xbc, 0x70, 0x10, 0xdc, 0x35, 0xf9, 0x99, 0x55, 0x4c, 0x80, 0xe0, 0x2c,
    0x38, 0xf4, 0x94, 0x58, 0x41, 0x8d, 0xed, 0x21, 0xc8, 0x04, 0x64, 0xa8, 0xb1, 0x7d, 0x1d, 0xd1,
    0xeb, 0x27, 0x47, 0x8b, 0x92, 0x5e, 0x3e, 0xf2, 0x1b, 0xd7, 0xb7, 0x7b, 0x62, 0xae, 0xce, 0x02,
    0x16, 0xda, 0xba, 0x76, 0x6f, 0xa3, 0xc3, 0x0f, 0xe6, 0x2a, 0x4a, 0x86, 0x9f, 0x53, 0x33, 0xff,
    0x87, 0x4b, 0x2b, 0xe7, 0xfe, 0x32, 0x52, 0x9e, 0x77, 0xbb, 0xdb, 0x17, 0x0e, 0xc2, 0xa2, 0x6e,
    0x7a, 0xb6, 0xd6, 0x1a, 0x03, 0xcf, 0xaf, 0x63, 0x8a, 0x46, 0x26, 0xea, 0xf3, 0x3f, 0x5f, 0x93,
    0xa9, 0x65, 0x05, 0xc9, 0xd0, 0x1c, 0x7c, 0xb0, 0x59, 0x95, 0xf5, 0x39, 0x20, 0xec, 0x8c, 0x40,
    0x54, 0x98, 0xf8, 0x34, 0x2d, 0xe1, 0x81, 0x4d, 0xa4, 0x68, 0x08, 0xc4, 0xdd, 0x11, 0x71, 0xbd,
};

const uint8_t kToDual[256] = {
    0x00, 0x7b, 0xaf, 0xd4, 0x99, 0xe2, 0x36, 0x4d, 0xfa, 0x81, 0x55, 0x2e, 0x63, 0x18, 0xcc, 0xb7,
    0x86, 0xfd, 0x29, 0x52, 0x1f, 0x64, 0xb0, 0xcb, 0x7c, 0x07, 0xd3, 0xa8, 0xe5, 0x9e, 0x4a, 0x31,
    0xec, 0x97, 0x43, 0x38, 0x75, 0x0e, 0xda, 0xa1, 0x16, 0x6d, 0xb9, 0xc2, 0x8f, 0xf4, 0x20, 0x5b,
    0x6a, 0x11, 0xc5, 0xbe, 0xf3, 0x88, 0x5c, 0x27, 0x90, 0xeb, 0x3f, 0x44, 0x09, 0x72, 0xa6, 0xdd,
    0xef, 0x94, 0x40, 0x3b, 0x76, 0x0d, 0xd9, 0xa2, 0x15, 0x6e, 0xba, 0xc1, 0x8c, 0xf7, 0x23, 0x58,
    0x69, 0x12, 0xc6, 0xbd, 0xf0, 0x8b, 0x5f, 0x24, 0x93, 0xe8, 0x3c, 0x47, 0x0a, 0x71, 0xa5, 0xde,
    0x03, 0x78, 0xac, 0xd7, 0x9a, 0xe1, 0x35, 0x4e, 0xf9, 0x82, 0x56, 0x2d, 0x60, 0x1b, 0xcf, 0xb4,
    0x85, 0xfe, 0x2a, 0x51, 0x1c, 0x67, 0xb3, 0xc8, 0x7f, 0x04, 0xd0, 0xab, 0xe6, 0x9d, 0x49, 0x32,
    0x8d, 0xf6, 0x22, 0x59, 0x14, 0x6f, 0xbb, 0xc0, 0x77, 0x0c, 0xd8, 0xa3, 0xee, 0x95, 0x41, 0x3a,
    0x0b, 0x70, 0xa4, 0xdf, 0x92, 0xe9, 0x3d, 0x46, 0xf1, 0x8a, 0x5e, 0x25, 0x68, 0x13, 0xc7, 0xbc,
    0x61, 0x1a, 0xce, 0xb5, 0xf8, 0x83, 0x57, 0x2c, 0x9b, 0xe0, 0x34, 0x4f, 0x02, 0x79, 0xad, 0xd6,
    0xe7, 0x9c, 0x48, 0x33, 0x7e, 0x05, 0xd1, 0xaa, 0x1d, 0x66, 0xb2, 0xc9, 0x84, 0xff, 0x2b, 0x50,
    0x62, 0x19, 0xcd, 0xb6, 0xfb, 0x80, 0x54, 0x2f, 0x98, 0xe3, 0x37, 0x4c, 0x01, 0x7a, 0xae, 0xd5,
    0xe4, 0x9f, 0x4b, 0x30, 0x7d, 0x06, 0xd2, 0xa9, 0x1e, 0x65, 0xb1, 0xca, 0x87, 0xfc, 0x28, 0x53,
    0x8e, 0xf5, 0x21, 0x5a, 0x17, 0x6c, 0xb8, 0xc3, 0x74, 0x0f, 0xdb, 0xa0, 0xed, 0x96, 0x42, 0x39,
    0x08, 0x73, 0xa7, 0xdc, 0x91, 0xea, 0x3e, 0x45, 0xf2, 0x89, 0x5d, 0x26, 0x6b, 0x10, 0xc4, 0xbf,
};

// Gaussian elimination over GF(256); false if singular.
bool solve_gf(std::vector<std::vector<uint8_t>>& a, std::vector<uint8_t>& b,
              std::vector<uint8_t>& x) {
    const int n = (int)b.size();
    for (int col = 0; col < n; ++col) {
        int piv = -1;
        for (int r = col; r < n; ++r)
            if (a[(size_t)r][(size_t)col]) { piv = r; break; }
        if (piv < 0) return false;
        std::swap(a[(size_t)col], a[(size_t)piv]);
        std::swap(b[(size_t)col], b[(size_t)piv]);
        const uint8_t inv = ginv(a[(size_t)col][(size_t)col]);
        for (int c = col; c < n; ++c) a[(size_t)col][(size_t)c] = gmul(a[(size_t)col][(size_t)c], inv);
        b[(size_t)col] = gmul(b[(size_t)col], inv);
        for (int r = 0; r < n; ++r) {
            if (r == col || !a[(size_t)r][(size_t)col]) continue;
            const uint8_t f = a[(size_t)r][(size_t)col];
            for (int c = col; c < n; ++c)
                a[(size_t)r][(size_t)c] ^= gmul(f, a[(size_t)col][(size_t)c]);
            b[(size_t)r] ^= gmul(f, b[(size_t)col]);
        }
    }
    x = b;
    return true;
}

// Berlekamp-Massey; returns the locator ascending with c[0]=1.
std::vector<uint8_t> berlekamp_massey(const uint8_t* s, int n, int max_degree) {
    std::vector<uint8_t> c(1, 1), b(1, 1);
    int l = 0, m = 1;
    uint8_t bb = 1;
    for (int i = 0; i < n; ++i) {
        uint8_t d = s[i];
        for (int j = 1; j <= l && j < (int)c.size(); ++j) d ^= gmul(c[(size_t)j], s[i - j]);
        if (!d) { ++m; continue; }
        const std::vector<uint8_t> t = c;
        const uint8_t coef = gmul(d, ginv(bb));
        if (c.size() < b.size() + (size_t)m) c.resize(b.size() + (size_t)m, 0);
        for (size_t j = 0; j < b.size(); ++j)
            c[j + (size_t)m] ^= gmul(coef, b[j]);
        if (2 * l <= i) {
            l = i + 1 - l;
            b = t;
            bb = d;
            m = 1;
        } else {
            ++m;
        }
    }
    if (l < 1 || l > max_degree) return {};
    c.resize((size_t)l + 1, 0);
    return c;
}

void chien(const std::vector<uint8_t>& c, std::vector<int>& degrees) {
    degrees.clear();
    for (int dd = 0; dd < kN; ++dd) {
        uint8_t acc = 0;
        for (size_t m = 0; m < c.size(); ++m) {
            if (!c[m]) continue;
            int e = (-kGap * dd * (int)m) % kOrder;
            if (e < 0) e += kOrder;
            acc ^= gmul(c[m], gf().exp[e]);
        }
        if (!acc) degrees.push_back(dd);
    }
}

}  // namespace

const std::vector<uint8_t>& ReedSolomon::generator_ascending() {
    static const std::vector<uint8_t> g = [] {
        std::vector<uint8_t> v{1};
        for (uint8_t root : roots()) {
            std::vector<uint8_t> ng(v.size() + 1, 0);
            for (size_t m = 0; m < v.size(); ++m) {
                ng[m] ^= gmul(v[m], root);
                ng[m + 1] ^= v[m];
            }
            v.swap(ng);
        }
        return v;
    }();
    return g;
}

void ReedSolomon::dual_basis_tables(uint8_t* to_dual, uint8_t* from_dual) {
    if (to_dual) std::memcpy(to_dual, kToDual, 256);
    if (from_dual) std::memcpy(from_dual, kFromDual, 256);
}

void ReedSolomon::encode_block(uint8_t* block) const {
    uint8_t buf[kN];
    std::memcpy(buf, block, kN);
    const std::vector<uint8_t>& g = generator_ascending();
    uint8_t gd[kParity + 1];
    for (int k = 0; k <= kParity; ++k) gd[k] = g[(size_t)(kParity - k)];

    uint8_t rem[kN];
    std::memcpy(rem, buf, kN);
    for (int i = kK; i < kN; ++i) rem[i] = 0;
    for (int i = 0; i < kK; ++i) {
        const uint8_t coef = rem[i];
        if (!coef) continue;
        for (int k = 0; k <= kParity; ++k) rem[i + k] ^= gmul(gd[k], coef);
    }
    for (int i = kK; i < kN; ++i) buf[i] = rem[i];
    if (dual_)
        for (int i = 0; i < kN; ++i) buf[i] = kToDual[buf[i]];
    std::memcpy(block, buf, kN);
}

int ReedSolomon::decode_block(uint8_t* block, const uint8_t* erase_pos, int n_erase) const {
    if (!block) return -1;
    if (n_erase < 0 || n_erase > kParity) return -1;

    uint8_t buf[kN];
    std::memcpy(buf, block, kN);
    if (dual_)
        for (int i = 0; i < kN; ++i) buf[i] = kFromDual[buf[i]];

    uint8_t s[kRoots];
    bool clean = true;
    for (int j = 0; j < kRoots; ++j) {
        uint8_t acc = 0;
        const uint8_t root = roots()[(size_t)j];
        for (int i = 0; i < kN; ++i) acc ^= gmul(buf[i], gpow(root, 254 - i));
        s[j] = acc;
        if (acc) clean = false;
    }
    if (clean) return 0;

    std::vector<uint8_t> gamma(1, 1);
    if (n_erase > 0) {
        gamma.assign(1, 1);
        for (int k = 0; k < n_erase; ++k) {
            const int dd = 254 - (int)erase_pos[k];      // Chien degree of this symbol
            const uint8_t x = gf().exp[(kGap * dd) % kOrder];
            std::vector<uint8_t> ng(gamma.size() + 1, 0);
            for (size_t m = 0; m < gamma.size(); ++m) {
                ng[m] ^= gamma[m];                        // *1
                ng[m + 1] ^= gmul(gamma[m], x);           // *x
            }
            gamma.swap(ng);
        }
    }
    const int ne = (int)gamma.size() - 1;
    if (2 * ne > kParity) return -1;

    uint8_t t[kRoots] = {};
    if (ne > 0) {
        uint8_t conv[kRoots * 2] = {};
        for (int i = 0; i < kRoots; ++i) {
            if (!s[i]) continue;
            for (size_t m = 0; m < gamma.size(); ++m)
                conv[i + (int)m] ^= gmul(s[i], gamma[m]);
        }
        for (int i = 0; i < kRoots - ne; ++i) t[i] = conv[i + ne];
    } else {
        std::memcpy(t, s, kRoots);
    }

    const std::vector<uint8_t> lambda_e =
        berlekamp_massey(t, kRoots - ne, (kParity - ne) / 2);
    if (lambda_e.empty()) return -1;

    std::vector<uint8_t> loc(lambda_e.size() + gamma.size() - 1, 0);
    for (size_t i = 0; i < lambda_e.size(); ++i)
        for (size_t j = 0; j < gamma.size(); ++j)
            loc[i + j] ^= gmul(lambda_e[i], gamma[j]);
    while (loc.size() > 1 && loc.back() == 0) loc.pop_back();

    std::vector<int> degrees;
    chien(loc, degrees);
    const int l = (int)loc.size() - 1;
    if ((int)degrees.size() != l || l <= 0 || l > kParity) return -1;

    std::vector<std::vector<uint8_t>> a((size_t)l, std::vector<uint8_t>((size_t)l, 0));
    std::vector<uint8_t> rhs((size_t)l, 0);
    for (int j = 0; j < l; ++j) {
        rhs[(size_t)j] = s[j];
        for (int k = 0; k < l; ++k)
            a[(size_t)j][(size_t)k] = gpow(roots()[(size_t)j], degrees[(size_t)k]);
    }
    std::vector<uint8_t> mag;
    if (!solve_gf(a, rhs, mag)) return -1;
    for (int k = 0; k < l; ++k) buf[254 - degrees[(size_t)k]] ^= mag[(size_t)k];

    // Only accept a word whose syndromes are all zero.
    for (int j = 0; j < kRoots; ++j) {
        uint8_t acc = 0;
        const uint8_t root = roots()[(size_t)j];
        for (int i = 0; i < kN; ++i) acc ^= gmul(buf[i], gpow(root, 254 - i));
        if (acc) return -1;
    }

    // Always return to the wire basis: mixing bases makes the output depend on whether a correction happened.
    if (dual_)
        for (int i = 0; i < kN; ++i) buf[i] = kToDual[buf[i]];
    std::memcpy(block, buf, kN);
    return l;
}

void ReedSolomon::decode_interleaved(uint8_t* data, int interleave,
                                     std::vector<int>& errors) const {
    errors.assign((size_t)std::max(0, interleave), -1);
    if (!data || interleave < 1 || interleave > 10) return;
    uint8_t buff[kN];
    for (int pos = 0; pos < interleave; ++pos) {
        for (int i = 0; i < kN; ++i) buff[i] = data[(size_t)(i * interleave + pos)];
        const int r = decode_block(buff);
        errors[(size_t)pos] = r;
        if (r >= 0)
            for (int i = 0; i < kN; ++i) data[(size_t)(i * interleave + pos)] = buff[i];
    }
}

}  // namespace eu
