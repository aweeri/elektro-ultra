#include "fec/fec_chain.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/common.h"
#include "core/parallel.h"

namespace eu {

bool FecChain::configure(int cadu_bits, int rs_depth, bool derandomise,
                         bool dual_basis, bool erasures, int rotations) {
    cadu_bits_ = cadu_bits;
    cadu_bytes_ = cadu_bits / 8;
    rs_depth_ = rs_depth;
    derand_ = derandomise;
    erasures_ = erasures;
    rotations_ = std::max(1, rotations);
    rot_ = 0;
    hunt_bits_ = 0;
    rs_.set_dual_basis(dual_basis);
    deframer_.configure(cadu_bits, 96);
    viterbi_.reset();
    st_ = FecStats{};
    flip_ = false;
    return true;
}

void FecChain::reset() {
    viterbi_.reset();
    deframer_.reset();
    st_ = FecStats{};
    bits_.clear();
    cadus_.clear();
    flip_ = false;
}

void FecChain::apply_hypothesis() {
    if (rot_ == 0 || buf_.size() < 2) return;
    if (rotations_ == 2) {
        for (auto& v : buf_) v = (int8_t)(-v);
        return;
    }
    const size_t n = buf_.size() & ~size_t(1);   // whole (I,Q) pairs only
    for (size_t i = 0; i < n; i += 2) {
        const int8_t a = buf_[i], b = buf_[i + 1];
        switch (rot_) {
            case 1: buf_[i] = b;  buf_[i + 1] = (int8_t)(-a); break;   // 90 deg
            case 2: buf_[i] = (int8_t)(-a); buf_[i + 1] = (int8_t)(-b); break;
            default: buf_[i] = (int8_t)(-b); buf_[i + 1] = a; break;   // 270 deg
        }
    }
}

void FecChain::push(const int8_t* soft, size_t n,
                    std::vector<std::vector<uint8_t>>& payloads) {
    if (!soft || n == 0) return;
    st_.symbols += n;
    hunt_bits_ += n;

    if (rot_ != 0 || flip_) {
        buf_.assign(soft, soft + n);
        if (flip_) for (auto& v : buf_) v = (int8_t)(-v);
        apply_hypothesis();
        soft = buf_.data();
    } else {
        buf_.clear();
    }

    hard_.clear();
    viterbi_.push(soft, n, hard_);
    if (hard_.empty()) return;

    if (const char* dp = std::getenv("EU_DUMP_VBITS")) {
        if (FILE* f = std::fopen(dp, "ab")) {
            std::fwrite(hard_.data(), 1, hard_.size(), f);
            std::fclose(f);
        }
    }

    // +-1, not 0/1: the deframer scores the ASM by correlation, so a frame with 3 wrong ASM bits still scores 0.81 and is kept for RS.
    bits_.resize(hard_.size());
    for (size_t i = 0; i < hard_.size(); ++i)
        bits_[i] = hard_[i] ? (int8_t)64 : (int8_t)-64;

    cadus_.clear();
    deframer_.push(bits_.data(), bits_.size(), cadus_);

    // Frames are independent once deframed, so derandomise + RS decode splits across threads.
    if (!cadus_.empty()) {
        const int nc = (int)cadus_.size();
        catu_.assign((size_t)nc, CatuOut{});
        if (nc == 1) handle_cadu(cadus_[0], catu_[0]);
        else parallel_for(nc, [&](int k) { handle_cadu(cadus_[(size_t)k], catu_[(size_t)k]); });
        for (int k = 0; k < nc; ++k) {
            CatuOut& r = catu_[(size_t)k];
            ++st_.cadus;
            if (r.state == 1) {
                st_.rs_corrected += (uint64_t)r.corrected;
                ++st_.cadus_good;
                payloads.push_back(std::move(r.payload));
            } else if (r.state == 2) {
                ++st_.rs_failed;
            }
        }
    }

    // No frame sync in a full window means the loop settled on the wrong phase; walk the rotations until frames appear, then never change.
    if (st_.cadus_good == 0 && !deframer_.locked() && rotations_ > 1 && hunt_bits_ >= (2u << 20)) {
        hunt_bits_ = 0;
        rot_ = (rot_ + 1) % rotations_;
        viterbi_.reset();
        deframer_.reset();
        if (rot_ == 0 && ++hunt_cycles_ <= 3)
            EU_LOGI("fec: no frame sync after %llu symbols, still hunting carrier phase",
                    (unsigned long long)st_.symbols);
    }

    st_.viterbi_ber = viterbi_.ber();
    st_.viterbi_locked = viterbi_.locked();
    st_.deframer_locked = deframer_.locked();
    st_.asm_confidence = deframer_.mean_confidence();
}

void FecChain::handle_cadu(const Cadu& c, CatuOut& out) const {
    out.state = 0;
    out.corrected = 0;
    out.payload.clear();
    if ((int)c.bytes.size() < cadu_bytes_) return;

    std::vector<uint8_t> cadu = c.bytes;

    // ASM is transmitted clear.
    if (derand_ && cadu_bytes_ > 4) rand_.apply(cadu.data() + 4, (size_t)cadu_bytes_ - 4, 0);

    const size_t rs_len = (size_t)cadu_bytes_ - 4;
    if (rs_depth_ <= 0 || rs_len < (size_t)rs_depth_ * ReedSolomon::kN) {
        out.payload = std::move(cadu);
        out.state = 1;
        return;
    }

    std::vector<int> errors;
    rs_.decode_interleaved(cadu.data() + 4, rs_depth_, errors);

    int corrected = 0;
    for (int e : errors) {
        if (e < 0) { out.state = 2; return; }
        corrected += e;
    }
    out.corrected = corrected;
    out.state = 1;
    // 892 bytes: 6-byte VCDU header + 886 bytes of data.
    out.payload.assign(cadu.begin() + 4, cadu.begin() + 4 + rs_depth_ * ReedSolomon::kK);
}

}  // namespace eu
