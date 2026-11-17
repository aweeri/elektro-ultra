#include "fec/deframer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/common.h"

namespace eu {

namespace {

constexpr uint8_t kAsmBits[32] = {
    0, 0, 0, 1, 1, 0, 1, 0,   // 0x1A
    1, 1, 0, 0, 1, 1, 1, 1,   // 0xCF
    1, 1, 1, 1, 1, 1, 0, 0,   // 0xFC
    0, 0, 0, 1, 1, 1, 0, 1,   // 0x1D
};

constexpr double kAcquireScore = 0.72;   // 0.5 is noise: 24/32 match by luck
constexpr double kTrackScore = 0.50;
constexpr int kConfirmFrames = 2;
// 20 misses before dropping sync, matching GobDump's viterbi_outsync_after; dropping after 4 starved the FEC at the noise edge.
constexpr int kMaxOutsync = 20;

// Overridable for sweeps; at low SNR this threshold, not the FEC, is the limit.
double acquire_score() {
    static const double v = [] {
        const char* s = std::getenv("EU_ASM_ACQ");
        return s ? std::atof(s) : kAcquireScore;
    }();
    return v;
}
double track_score() {
    static const double v = [] {
        const char* s = std::getenv("EU_ASM_TRACK");
        return s ? std::atof(s) : kTrackScore;
    }();
    return v;
}

}  // namespace

void Deframer::configure(int cadu_bits, int hunt) {
    cadu_bits_ = cadu_bits;
    cadu_bytes_ = cadu_bits / 8;
    hunt_ = std::max(16, hunt);
    reset();
}

void Deframer::reset() {
    bits_.clear();
    search_from_ = 0;
    next_expect_ = -1;
    locked_ = false;
    inverted_ = false;
    good_run_ = 0;
    miss_run_ = 0;
    cand_ = -1;
    cand_score_ = 0.0;
    cand_inv_ = false;
    frames_ = 0;
    conf_acc_ = 0.0;
    conf_n_ = 0;
}

int Deframer::hunt_range(size_t from, size_t to, double* best_score, bool* best_inv) const {
    if (bits_.size() < 32) return -1;
    to = std::min(to, bits_.size() - 31);
    if (from >= to) return -1;
    int best_pos = -1;
    // |score| is in [0,1]: seeding |best| >= 1 blocks every candidate.
    double best_abs = -1.0;
    double best_signed = 0.0;
    bool best_neg = false;
    if (std::getenv("EU_DEFRAME_DEBUG")) {
        static int shown = 0;
        if (shown++ < 3)
            std::fprintf(stderr, "  hunt_range size %zu from %zu to %zu vals %d %d %d\n",
                         bits_.size(), from, to, (int)bits_[0], (int)bits_[1], (int)bits_[2]);
    }
    for (size_t p = from; p < to; ++p) {
        double acc = 0.0, norm = 0.0;
        for (int k = 0; k < 32; ++k) {
            const double v = (double)bits_[p + (size_t)k];
            const double s = kAsmBits[k] ? 1.0 : -1.0;
            acc += v * s;
            norm += std::fabs(v);
        }
        if (norm < 1e-9) continue;
        const double score = acc / norm;
        if (std::fabs(score) > best_abs) {
            best_abs = std::fabs(score);
            best_signed = score;
            best_pos = (int)p;
            best_neg = score < 0.0;
        }
    }
    (void)best_signed;
    if (best_score) *best_score = best_abs;
    if (best_inv) *best_inv = best_neg;
    return best_pos;
}

bool Deframer::emit(size_t pos, bool inv, double score, std::vector<Cadu>& out) {
    if (pos + (size_t)cadu_bits_ > bits_.size()) return false;
    Cadu c;
    c.bytes.assign((size_t)cadu_bytes_, 0);
    for (int i = 0; i < cadu_bits_; ++i) {
        int bit = bits_[pos + (size_t)i] > 0 ? 1 : 0;
        if (inv) bit ^= 1;
        if (bit) c.bytes[(size_t)(i >> 3)] |= (uint8_t)(1u << (7 - (i & 7)));
    }
    if (std::getenv("EU_DEFRAME_DEBUG")) {
        std::fprintf(stderr, "deframe: cadu at bit %zu conf %.3f inv %d\n",
                     pos, score, (int)inv);
    }
    c.confidence = score;
    c.inverted = inv;
    c.index = frames_++;
    conf_acc_ += score;
    conf_n_ += 1;
    out.push_back(std::move(c));
    return true;
}

void Deframer::push(const int8_t* soft, size_t n, std::vector<Cadu>& out) {
    if (!soft || n == 0) return;
    bits_.insert(bits_.end(), soft, soft + n);
    if (std::getenv("EU_DEFRAME_DEBUG")) {
        static int calls = 0;
        if (calls++ < 8)
            std::fprintf(stderr, "deframer: push %zu bits, have %zu, from %zu, locked %d\n",
                         n, bits_.size(), search_from_, (int)locked_);
    }

    for (;;) {
        if (!locked_) {
            double score = 0.0;
            bool inv = inverted_;

            // Coasting was tried and removed: 3x the frames attempted at the noise edge, zero extra frames decoded.
            const int pos = hunt_range(search_from_, bits_.size(), &score, &inv);
            if (pos < 0) break;
            if (score < acquire_score()) {
                search_from_ = bits_.size() > 31 ? bits_.size() - 31 : 0;
                break;
            }
            if ((size_t)pos + (size_t)cadu_bits_ > bits_.size()) {
                // Body not in yet: wait rather than step past a known-good sync.
                search_from_ = (size_t)pos;
                break;
            }
            if (ever_locked_ && (size_t)pos + (size_t)cadu_bits_ <= bits_.size()) {
                emit((size_t)pos, inv, score, out);
                inverted_ = inv;
                locked_ = true;
                ever_locked_ = true;
                good_run_ = kConfirmFrames;
                next_expect_ = (long long)pos + cadu_bits_;
                search_from_ = (size_t)pos + 1;
                cand_ = -1;
            } else if (cand_ >= 0 &&
                std::llabs((long long)pos - (cand_ + (long long)cadu_bits_)) <= (long long)hunt_) {
                if (!emit((size_t)cand_, cand_inv_, cand_score_, out)) {
                    search_from_ = (size_t)cand_;
                    break;
                }
                emit((size_t)pos, inv, score, out);
                inverted_ = inv;
                locked_ = true;
                ever_locked_ = true;
                good_run_ = kConfirmFrames;
                next_expect_ = (long long)pos + cadu_bits_;
                search_from_ = (size_t)pos + 1;
                cand_ = -1;
            } else {
                cand_ = (long long)pos;
                cand_score_ = score;
                cand_inv_ = inv;
                search_from_ = (size_t)pos + 1;
            }
        } else {
            if (next_expect_ < 0) break;
            const long long lo = next_expect_ - hunt_;
            const long long hi = next_expect_ + hunt_ + 32;
            if (hi > (long long)bits_.size()) break;
            double score = 0.0;
            bool inv = inverted_;
            const int pos = hunt_range((size_t)std::max(0LL, lo), (size_t)hi, &score, &inv);
            if (pos >= 0 && score >= track_score()) {
                if (!emit((size_t)pos, inv, score, out)) {
                    next_expect_ = (long long)pos;
                    break;
                }
                inverted_ = inv;
                next_expect_ = (long long)pos + cadu_bits_;
                search_from_ = (size_t)pos + 1;
                miss_run_ = 0;
                if (++good_run_ >= kConfirmFrames) locked_ = true;
            } else {
                ++miss_run_;
                if (miss_run_ > kMaxOutsync) {
                    locked_ = false;
                    good_run_ = 0;
                    cand_ = -1;
                    search_from_ = (size_t)std::max(0LL, next_expect_ - hunt_);
                    miss_run_ = 0;
                } else {
                    next_expect_ += cadu_bits_;
                }
            }
        }

        // Keep one frame of overlap when trimming.
        const size_t keep = (size_t)std::max(0LL,
            (next_expect_ > 0 ? next_expect_ - hunt_ : (long long)search_from_));
        if (keep > (1u << 18) && bits_.size() > keep) {
            bits_.erase(bits_.begin(), bits_.begin() + (long)keep);
            search_from_ = search_from_ > keep ? search_from_ - keep : 0;
            if (next_expect_ > 0) next_expect_ -= (long long)keep;
        }
    }
}

void Deframer::flush(std::vector<Cadu>& out) { (void)out; }

}  // namespace eu
