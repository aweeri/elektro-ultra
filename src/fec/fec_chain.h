// Viterbi (rate 1/2, K=7) -> soft-ASM deframe -> derandomise -> depth-4 deinterleave -> RS(255,223) dual basis.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fec/deframer.h"
#include "fec/randomiser.h"
#include "fec/rs.h"
#include "fec/viterbi.h"

namespace eu {

struct FecStats {
    uint64_t symbols = 0;
    uint64_t cadus = 0;            // frames that reached RS
    uint64_t cadus_good = 0;       // RS clean or corrected
    uint64_t rs_corrected = 0;     // symbols corrected, summed
    uint64_t rs_failed = 0;
    uint64_t polarity_flips = 0;
    double   viterbi_ber = 1.0;
    double   asm_confidence = 0.0;
    bool     viterbi_locked = false;
    bool     deframer_locked = false;
};

class FecChain {
public:
    bool configure(int cadu_bits = 8192, int rs_depth = 4, bool derandomise = true,
                   bool dual_basis = true, bool erasures = true, int rotations = 2);

    // Appends the 892-byte payload of every good frame.
    void push(const int8_t* soft, size_t n, std::vector<std::vector<uint8_t>>& payloads);

    const FecStats& stats() const { return st_; }
    void reset();

private:
    struct CatuOut {
        int  state = 0;      // 0 short, 1 good, 2 uncorrectable
        int  corrected = 0;
        std::vector<uint8_t> payload;
    };
    void handle_cadu(const Cadu& c, CatuOut& out) const;

    Viterbi      viterbi_;
    Deframer     deframer_;
    Randomiser   rand_;
    ReedSolomon  rs_{true};

    int  cadu_bits_ = 8192;
    int  cadu_bytes_ = 1024;
    int  rs_depth_ = 4;
    bool derand_ = true;
    bool erasures_ = true;
    bool flip_ = false;
    // The ASM is inside the coded data, so a wrong carrier phase is invisible until after the Viterbi; each rotation is tried in turn.
    int      rotations_ = 2;
    int      rot_ = 0;
    uint64_t hunt_bits_ = 0;
    unsigned hunt_cycles_ = 0;
    std::vector<int8_t> buf_;
    void apply_hypothesis();

    std::vector<uint8_t> hard_;     // Viterbi output, one bit per byte
    std::vector<int8_t>  bits_;     // +-1 for the ASM correlator
    std::vector<Cadu>    cadus_;
    std::vector<CatuOut> catu_;
    FecStats st_;
};

}  // namespace eu
