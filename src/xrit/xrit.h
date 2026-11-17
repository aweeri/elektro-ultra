// xRIT file headers (CCSDS/EUMETSAT LRIT-HRIT layout, Elektro variant).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eu {

struct XritHeaders {
    int      file_type_code = -1;      // 0 = image, 128 = text, 129 = ...
    uint32_t total_header_length = 0;
    uint64_t data_field_length = 0;

    // Image structure record, when present.
    bool has_image = false;
    int  bits_per_pixel = 0;
    int  columns = 0;
    int  lines = 0;
    int  compression = -1;             // 0 none, 1 wavelet, 2 JPEG

    // Annotation record: the product filename.
    std::string annotation;
    uint64_t timestamp = 0;

    // Derived: channel and segment indices from the Elektro file name.
    int channel = -1;
    int segment = 0;

    bool valid = false;
};

// Parses the header chain out of a raw xRIT file image.
bool parse_xrit_headers(const std::vector<uint8_t>& file, XritHeaders& out);
bool parse_xrit_headers(const uint8_t* p, size_t n, XritHeaders& out);

}  // namespace eu
