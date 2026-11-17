// CCSDS RS(255,223), depth 4, dual basis; GF(2^8) p=x^8+x^7+x^2+x+1, roots alpha^(11*(112+j)), no Forney so a wrong answer fails the syndrome re-check.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eu {

class ReedSolomon {
public:
    static constexpr int kN = 255;
    static constexpr int kK = 223;
    static constexpr int kParity = 32;
    static constexpr int kMaxCorrectable = 16;

    explicit ReedSolomon(bool dual_basis = true) : dual_(dual_basis) {}
    void set_dual_basis(bool v) { dual_ = v; }
    bool dual_basis() const { return dual_; }

    // Corrected symbol count, 0 if clean, -1 if uncorrectable.
    // erase_pos holds symbol indices (0..254); 2*errors + erasures <= 32.
    int decode_block(uint8_t* block, const uint8_t* erase_pos = nullptr,
                     int n_erase = 0) const;

    // Strided deinterleave + decode in place; errors[i] = corrections or -1.
    void decode_interleaved(uint8_t* data, int interleave,
                            std::vector<int>& errors) const;

    void encode_block(uint8_t* block) const;

    static const std::vector<uint8_t>& generator_ascending();
    static void dual_basis_tables(uint8_t* to_dual, uint8_t* from_dual);

private:
    bool dual_ = true;
};

}  // namespace eu
