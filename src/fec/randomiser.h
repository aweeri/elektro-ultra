// CCSDS 131.0-B 10.3: h=x^8+x^7+x^5+x^3+1, all-ones seed, period 255, restarted per codeblock; taps 0x95, not 0xA9, reproduces the standard vector.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace eu {

class Randomiser {
public:
    Randomiser();

    void apply(uint8_t* data, size_t len, size_t offset = 0) const;

    const std::array<uint8_t, 255>& table() const { return table_; }

private:
    std::array<uint8_t, 255> table_{};
};

}  // namespace eu
