#include "xrit/png.h"

#include <cstdio>
#include <cstring>

#include <zlib.h>

#include "core/common.h"

namespace eu {

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t>& out, const char type[4],
           const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), &out[at], (uInt)(4 + data.size()));
    put32(out, (uint32_t)crc);
}

bool write_all(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

bool emit(const std::string& path, int cols, int rows, int bit_depth,
          int color_type, const std::vector<uint8_t>& raw) {
    if (cols <= 0 || rows <= 0) return false;

    std::vector<uint8_t> z;
    uLongf cap = compressBound((uLong)raw.size());
    z.resize(cap);
    if (compress2(z.data(), &cap, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    z.resize(cap);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)cols);
    put32(ihdr, (uint32_t)rows);
    ihdr.push_back((uint8_t)bit_depth);
    ihdr.push_back((uint8_t)color_type);
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return write_all(path, out);
}

// Prefix every scanline with filter type 0.
std::vector<uint8_t> with_filter(const uint8_t* data, size_t row_bytes, int rows) {
    std::vector<uint8_t> raw((size_t)rows * (row_bytes + 1));
    for (int y = 0; y < rows; ++y) {
        raw[(size_t)y * (row_bytes + 1)] = 0;
        std::memcpy(&raw[(size_t)y * (row_bytes + 1) + 1], data + (size_t)y * row_bytes,
                    row_bytes);
    }
    return raw;
}

}  // namespace

bool png_write_gray8(const std::string& path, int cols, int rows,
                     const uint8_t* data) {
    if (!data) return false;
    return emit(path, cols, rows, 8, 0, with_filter(data, (size_t)cols, rows));
}

bool png_write_gray16(const std::string& path, int cols, int rows,
                      const std::vector<uint16_t>& data) {
    if ((int)data.size() < cols * rows) return false;
    std::vector<uint8_t> be((size_t)cols * rows * 2);
    for (size_t i = 0; i < (size_t)cols * rows; ++i) {
        be[i * 2] = (uint8_t)(data[i] >> 8);
        be[i * 2 + 1] = (uint8_t)data[i];
    }
    return emit(path, cols, rows, 16, 0, with_filter(be.data(), (size_t)cols * 2, rows));
}

bool png_write_rgb8(const std::string& path, int cols, int rows,
                    const std::vector<uint8_t>& rgb) {
    if (rgb.size() < (size_t)cols * rows * 3) return false;
    return emit(path, cols, rows, 8, 2, with_filter(rgb.data(), (size_t)cols * 3, rows));
}

}  // namespace eu
