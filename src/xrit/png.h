// Minimal PNG writer (zlib). 8 or 16 bit, grayscale or RGB.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eu {

// Writes an 8-bit greyscale PNG. `data` is rows*cols bytes.
bool png_write_gray8(const std::string& path, int cols, int rows,
                     const uint8_t* data);

// 16-bit greyscale, host order in, big-endian on disk.
bool png_write_gray16(const std::string& path, int cols, int rows,
                      const std::vector<uint16_t>& data);

// 8-bit RGB, rows*cols*3 bytes.
bool png_write_rgb8(const std::string& path, int cols, int rows,
                    const std::vector<uint8_t>& rgb);

}  // namespace eu
