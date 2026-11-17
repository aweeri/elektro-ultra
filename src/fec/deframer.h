// ASM is correlated against soft bits rather than matched exactly, so a marginal frame still yields a score and polarity falls out of the sign.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eu {

struct Cadu {
    std::vector<uint8_t> bytes;    // ASM included, always normal polarity
    double confidence = 0.0;       // 0..1 normalised ASM correlation
    bool   inverted = false;       // polarity the frame arrived with
    uint64_t index = 0;
};

class Deframer {
public:
    void configure(int cadu_bits = 8192, int hunt = 96);
    void reset();

    void push(const int8_t* soft, size_t n, std::vector<Cadu>& out);
    void flush(std::vector<Cadu>& out);

    bool locked() const { return locked_; }
    uint64_t frames() const { return frames_; }
    double mean_confidence() const { return conf_n_ ? conf_acc_ / (double)conf_n_ : 0.0; }

private:
    // Best correlation in [from, to); -1 if none.
    int hunt_range(size_t from, size_t to, double* best_score, bool* best_inv) const;
    bool emit(size_t pos, bool inv, double score, std::vector<Cadu>& out);

    int cadu_bits_ = 8192;
    int cadu_bytes_ = 1024;
    int hunt_ = 96;

    std::vector<int8_t> bits_;
    size_t search_from_ = 0;
    long long next_expect_ = -1;  // where the next ASM should be

    bool  locked_ = false;
    bool  ever_locked_ = false;   // after one confirmed lock a single ASM is trusted
    bool  inverted_ = false;
    int   good_run_ = 0;
    int   miss_run_ = 0;
    // Held until a second ASM appears at the expected spacing: noise produces 32-bit correlations above 0.7 often enough to invent whole frames.
    long long cand_ = -1;
    double    cand_score_ = 0.0;
    bool      cand_inv_ = false;
    uint64_t frames_ = 0;
    double conf_acc_ = 0.0;
    uint64_t conf_n_ = 0;
};

}  // namespace eu
