// CCSDS rate 1/2, K=7, G1=171oct, G2=133oct; soft in (int8, positive favours 1), hard out, block traceback so metrics carry across calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eu {

class Viterbi {
public:
    Viterbi(int g1 = 0171, int g2 = 0133);

    void reset();

    void push(const int8_t* soft, size_t n, std::vector<uint8_t>& out);

    // Reference encoder for synthetic tests and BER.
    static void encode(const uint8_t* bits, size_t nbits, int g1, int g2,
                       std::vector<uint8_t>& out_bits);

    double ber() const { return ber_; }
    bool locked() const { return locked_; }
    uint64_t bits_out() const { return bits_out_; }

private:
    void process_block(const int8_t* soft, size_t pairs, std::vector<uint8_t>& out,
                       bool final_block);

    int g1_, g2_;
    static constexpr int kStates = 64;
    static constexpr int kBlock = 1024;   // symbol pairs per traceback

    // int16 suffices: metrics are renormalised every 16 symbols.
    alignas(64) int16_t metric_[kStates] = {};
    alignas(64) int16_t next_[kStates] = {};
    // Output-bit patterns packed into a 4-entry per-symbol table.
    alignas(64) uint8_t bm_idx_[kStates * 2] = {};
    std::vector<uint8_t> dec_;            // kBlock * 64 predecessor bits
    std::vector<uint8_t> dst_;            // traceback scratch
    std::vector<int8_t> pend_;            // unpaired soft symbols

    uint8_t o1_[kStates * 2] = {};        // output bit 1 for (s', x)
    uint8_t o2_[kStates * 2] = {};

    uint64_t ber_bad_ = 0, ber_tot_ = 0;
    double ber_ = 1.0;
    bool locked_ = false;
    uint64_t bits_out_ = 0;
};

}  // namespace eu
