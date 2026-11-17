// CADU payload -> VCDU (6-byte header, 2-byte M_PDU header, 884 bytes) -> CCSDS packets; sequence flags delimit xRIT files and the first packet carries a 10-byte transport header that is not part of the file.
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace eu {

struct XritBlob {
    uint8_t vcid = 0;
    uint16_t apid = 0;
    std::vector<uint8_t> data;
};

class TransportDemux {
public:
    static constexpr size_t kPayload = 892;
    static constexpr size_t kDataField = 884;

    void reset();
    void push(const uint8_t* payload, size_t n, std::vector<XritBlob>& out);

    uint64_t packets() const { return packets_; }
    uint64_t files() const { return files_done_; }
    uint64_t crc_bad() const { return crc_bad_; }

private:
    struct Vc {
        std::vector<uint8_t> stream;
        size_t parse_pos = 0;
        uint64_t base = 0;
    };
    struct File {
        std::vector<uint8_t> data;
        bool in_progress = false;
    };

    void emit_packet(int vcid, const uint8_t* pkt, size_t total,
                     std::vector<XritBlob>& out);

    std::map<int, Vc> vcs_;
    std::map<std::pair<int, int>, File> files_;
    uint64_t packets_ = 0;
    uint64_t files_done_ = 0;
    uint64_t crc_bad_ = 0;
};

// CRC-16/CCITT as used by the LRIT transport layer.
uint16_t lrit_crc16(const uint8_t* data, size_t n);

}  // namespace eu
