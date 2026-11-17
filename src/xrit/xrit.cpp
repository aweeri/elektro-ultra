#include "xrit/xrit.h"

#include <cstring>

#include "core/common.h"

namespace eu {

namespace {

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// Elektro names: platform, -000-, instrument, level, channel_grid_area, segment, timestamp, compression/type.
void parse_name(const std::string& name, XritHeaders& h) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '-') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    if (parts.size() >= 6) {
        // Leading number of "03_8_076E".
        const std::string& f = parts[4];
        int ch = 0;
        size_t i = 0;
        while (i < f.size() && f[i] >= '0' && f[i] <= '9') {
            ch = ch * 10 + (f[i] - '0');
            ++i;
        }
        h.channel = (i > 0) ? ch : -1;
        // Numeric prefix of the segment field.
        const std::string& s = parts[5];
        int seg = 0;
        size_t j = 0;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            seg = seg * 10 + (s[j] - '0');
            ++j;
        }
        h.segment = seg;
    }
}

}  // namespace

bool parse_xrit_headers(const std::vector<uint8_t>& file, XritHeaders& out) {
    return parse_xrit_headers(file.data(), file.size(), out);
}

bool parse_xrit_headers(const uint8_t* p, size_t n, XritHeaders& out) {
    out = XritHeaders{};
    if (!p || n < 16) return false;
    if (p[0] != 0) return false;
    const uint32_t hdr_len = be32(p + 4);
    if (hdr_len < 16 || hdr_len > n) return false;

    out.file_type_code = p[3];
    out.total_header_length = hdr_len;
    out.data_field_length = be64(p + 8);

    for (uint32_t i = 0; i + 3 <= hdr_len;) {
        const uint8_t type = p[i];
        const uint16_t rlen = be16(p + i + 1);
        if (rlen < 3 || i + rlen > hdr_len) break;

        if (type == 1 && rlen >= 9) {          // image structure
            out.has_image = true;
            out.bits_per_pixel = p[i + 3];
            out.columns = be16(p + i + 4);
            out.lines = be16(p + i + 6);
            out.compression = p[i + 8];
        } else if (type == 2 && rlen >= 51) {  // image navigation, unused
        } else if (type == 4) {                // annotation
            const char* s = (const char*)(p + i + 3);
            size_t len = rlen - 3;
            while (len && s[len - 1] == '\0') --len;
            out.annotation.assign(s, len);
        } else if (type == 7 && rlen >= 10) {  // timestamp
            out.timestamp = (uint64_t)be32(p + i + 3) * 86400ULL + be32(p + i + 7);
        }
        i += rlen;
    }

    if (!out.annotation.empty()) parse_name(out.annotation, out);
    out.valid = true;
    return true;
}

}  // namespace eu
