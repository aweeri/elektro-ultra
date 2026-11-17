#include "fec/viterbi.h"

#include <algorithm>
#include <cstring>

namespace eu {

namespace {

inline int parity7(int v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

}  // namespace

Viterbi::Viterbi(int g1, int g2) : g1_(g1 & 0x7F), g2_(g2 & 0x7F) {
    dec_.assign((size_t)kBlock * kStates, 0);
    reset();
}

void Viterbi::reset() {
    std::fill(metric_, metric_ + kStates, (int16_t)0);
    std::fill(next_, next_ + kStates, (int16_t)0);
    // Start state unknown: a wrong guess costs one block.
    std::fill(dec_.begin(), dec_.end(), (uint8_t)0);
    pend_.clear();
    ber_bad_ = 0;
    ber_tot_ = 0;
    ber_ = 1.0;
    locked_ = false;
    bits_out_ = 0;

    // Generator window for state s' and bit x: (b<<6) | ((s'&0x1F)<<1 | x).
    for (int sp = 0; sp < kStates; ++sp) {
        const int b = sp >> 5;
        for (int x = 0; x < 2; ++x) {
            const int s = ((sp & 0x1F) << 1) | x;
            const int reg = (b << 6) | s;
            o1_[sp * 2 + x] = (uint8_t)parity7(reg & g1_);
            o2_[sp * 2 + x] = (uint8_t)parity7(reg & g2_);
            bm_idx_[sp * 2 + x] = (uint8_t)((o1_[sp * 2 + x] << 1) | o2_[sp * 2 + x]);
        }
    }
}

void Viterbi::encode(const uint8_t* bits, size_t nbits, int g1, int g2,
                     std::vector<uint8_t>& out_bits) {
    int s = 0;
    for (size_t i = 0; i < nbits; ++i) {
        const int b = bits[i] & 1;
        const int reg = (b << 6) | s;
        out_bits.push_back((uint8_t)parity7(reg & g1));
        out_bits.push_back((uint8_t)parity7(reg & g2));
        s = (s >> 1) | (b << 5);
    }
}

void Viterbi::push(const int8_t* soft, size_t n, std::vector<uint8_t>& out) {
    if (!soft || n == 0) return;
    pend_.insert(pend_.end(), soft, soft + n);
    size_t off = 0;
    while (pend_.size() - off >= (size_t)kBlock * 2) {
        process_block(pend_.data() + off, kBlock, out, false);
        off += (size_t)kBlock * 2;
    }
    if (off > 0) pend_.erase(pend_.begin(), pend_.begin() + (long)off);
}

void Viterbi::process_block(const int8_t* soft, size_t pairs,
                            std::vector<uint8_t>& out, bool final_block) {
    const size_t n = pairs;
    if (n == 0) return;

    for (size_t t = 0; t < n; ++t) {
        const int s0 = soft[t * 2];
        const int s1 = soft[t * 2 + 1];
        // Four branch metrics indexed by (o1<<1)|o2: one byte lookup, no multiplies.
        const int bm[4] = {-s0 - s1, -s0 + s1, s0 - s1, s0 + s1};
        uint8_t* dec = &dec_[t * kStates];

        // Butterfly: k and k+32 share predecessors.
        for (int k = 0; k < 32; ++k) {
            const int p0 = 2 * k, p1 = p0 + 1;
            const int a = metric_[p0], b = metric_[p1];
            const int i0 = bm_idx_[2 * k], i0b = bm_idx_[2 * k + 1];
            const int i1 = bm_idx_[2 * (k + 32)], i1b = bm_idx_[2 * (k + 32) + 1];
            const int m0a = a + bm[i0], m0b = b + bm[i0b];
            if (m0a >= m0b) { next_[k] = (int16_t)m0a; dec[k] = 0; }
            else            { next_[k] = (int16_t)m0b; dec[k] = 1; }
            const int m1a = a + bm[i1], m1b = b + bm[i1b];
            if (m1a >= m1b) { next_[k + 32] = (int16_t)m1a; dec[k + 32] = 0; }
            else            { next_[k + 32] = (int16_t)m1b; dec[k + 32] = 1; }
        }
        std::memcpy(metric_, next_, sizeof(metric_));

        // Subtract the minimum: bounds the spread so 16 bits suffice.
        if ((t & 0xF) == 0xF) {
            int16_t mn = metric_[0];
            for (int i = 1; i < kStates; ++i) mn = metric_[i] < mn ? metric_[i] : mn;
            if (mn != 0)
                for (int i = 0; i < kStates; ++i) metric_[i] = (int16_t)(metric_[i] - mn);
        }
    }

    int best = 0;
    for (int i = 1; i < kStates; ++i)
        if (metric_[i] > metric_[best]) best = i;

    // Scratch then one append: resize() zero-fills bytes the traceback immediately overwrites, which was 20 % of decode time.
    if (dst_.size() < n) dst_.resize(n);
    uint8_t* dst = dst_.data();
    int cur = best;
    for (size_t t = n; t-- > 0;) {
        dst[t] = (uint8_t)((cur >> 5) & 1);
        cur = ((cur & 0x1F) << 1) | dec_[t * kStates + cur];
    }
    out.insert(out.end(), dst, dst + n);
    bits_out_ += n;

    // BER: re-encode from the end state and compare with the input hard bits.
    {
        int st = cur;
        size_t bad = 0, total = 0;
        for (size_t t = 0; t < n; ++t) {
            const int b = dst[t];
            const int reg = (b << 6) | st;
            const int e1 = parity7(reg & g1_);
            const int e2 = parity7(reg & g2_);
            bad += (size_t)(e1 != (soft[t * 2] > 0));
            bad += (size_t)(e2 != (soft[t * 2 + 1] > 0));
            total += 2;
            st = (st >> 1) | (b << 5);
        }
        const double blk = total ? (double)bad / (double)total : 0.0;
        (void)blk;
        // Cumulative, not EWMA: a seeded EWMA reads 0.4 % on a clean stream.
        ber_bad_ += bad;
        ber_tot_ += total;
        ber_ = ber_tot_ ? (double)ber_bad_ / (double)ber_tot_ : 1.0;
        locked_ = ber_ < 0.10;
    }
    if (final_block) pend_.clear();
}

}  // namespace eu
