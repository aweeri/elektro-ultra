#include "fec/randomiser.h"

namespace eu {

namespace {

// Feedback taps that reproduce ff 48 0e c0 9a 0d 70 bc (equivalently h(x)=x^8+x^7+x^4+x^2+1 with this shift direction).
constexpr uint8_t kTaps = 0x95;

uint8_t parity8(uint8_t v) {
    v ^= (uint8_t)(v >> 4);
    v &= 0x0F;
    v ^= (uint8_t)(v >> 2);
    v &= 0x03;
    v ^= (uint8_t)(v >> 1);
    return (uint8_t)(v & 1u);
}

}  // namespace

Randomiser::Randomiser() {
    uint8_t state = 0xFF;
    for (size_t i = 0; i < table_.size(); ++i) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; ++b) {
            const uint8_t out = (uint8_t)((state >> 7) & 1u);
            const uint8_t fb = parity8((uint8_t)(state & kTaps));
            state = (uint8_t)((state << 1) | fb);
            byte = (uint8_t)((byte << 1) | out);
        }
        table_[i] = byte;
    }
}

void Randomiser::apply(uint8_t* data, size_t len, size_t offset) const {
    if (!data) return;
    for (size_t i = 0; i < len; ++i)
        data[i] ^= table_[(offset + i) % table_.size()];
}

}  // namespace eu
