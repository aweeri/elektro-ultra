#include "xrit/transport.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/common.h"

namespace eu {

namespace {
constexpr uint16_t kCrcTable[16] = {0};
}  // namespace

uint16_t lrit_crc16(const uint8_t* data, size_t n) {
    static uint16_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            uint16_t crc = (uint16_t)(i << 8);
            for (int b = 0; b < 8; ++b)
                crc = (uint16_t)((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1));
            table[i] = crc;
        }
        init = true;
    }
    (void)kCrcTable;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i)
        crc = (uint16_t)((crc << 8) ^ table[(uint8_t)((crc >> 8) ^ data[i])]);
    return crc;
}

void TransportDemux::reset() {
    vcs_.clear();
    files_.clear();
    packets_ = 0;
    files_done_ = 0;
    crc_bad_ = 0;
}

void TransportDemux::push(const uint8_t* payload, size_t n, std::vector<XritBlob>& out) {
    if (!payload || n < kPayload) return;

    const int vcid = payload[1] & 0x3F;
    if (vcid == 63) return;   // filler

    const uint16_t fhp = (uint16_t)(((payload[6] & 0x07) << 8) | payload[7]);
    const uint8_t* data = payload + 8;
    const size_t dsize = kDataField;

    Vc& vc = vcs_[vcid];
    const uint64_t base = (uint64_t)vc.stream.size();
    vc.stream.insert(vc.stream.end(), data, data + dsize);

    // The M_PDU pointer is a resync hint only: applying it unconditionally jumps over the tail of a packet spanning this boundary and loses most of the stream.
    if (fhp != 2047 && fhp < dsize && vc.parse_pos == base)
        vc.parse_pos = (size_t)base + fhp;

    while (vc.parse_pos + 6 <= vc.stream.size()) {
        const uint8_t* p = vc.stream.data() + vc.parse_pos;
        const size_t plen = ((size_t)p[4] << 8 | p[5]) + 1;
        const size_t total = 6 + plen;
        if (vc.parse_pos + total > vc.stream.size()) break;
        emit_packet(vcid, p, total, out);
        vc.parse_pos += total;
    }

    // Keep one packet of overlap.
    if (vc.parse_pos > (1u << 16)) {
        vc.stream.erase(vc.stream.begin(), vc.stream.begin() + (long)vc.parse_pos);
        vc.parse_pos = 0;
    }
}

void TransportDemux::emit_packet(int vcid, const uint8_t* pkt, size_t total,
                                 std::vector<XritBlob>& out) {
    const uint16_t apid = (uint16_t)(((pkt[0] & 0x07) << 8) | pkt[1]);
    const int seq = (pkt[2] >> 6) & 3;
    if (apid == 2047) return;          // idle

    const uint8_t* pl = pkt + 6;
    const size_t plen = total - 6;
    if (plen < 3) return;
    ++packets_;
    if (std::getenv("EU_PKT_DEBUG"))
        std::fprintf(stderr, "pkt vcid %d apid %u seq %d cnt %u len %zu\n", vcid, apid, seq,
                     (unsigned)(((pkt[2] & 0x3F) << 8) | pkt[3]), plen);

    const uint16_t crc = (uint16_t)((pl[plen - 2] << 8) | pl[plen - 1]);
    const bool crc_ok = crc == lrit_crc16(pl, plen - 2);
    if (!crc_ok) ++crc_bad_;

    auto& f = files_[std::make_pair(vcid, (int)apid)];

    // Sequence flags: 1 first, 2 last, 3 single, 0 continuation.
    if (seq == 1 || seq == 3) {
        if (f.in_progress && !f.data.empty()) {
            XritBlob b;
            b.vcid = (uint8_t)vcid;
            b.apid = apid;
            b.data = std::move(f.data);
            out.push_back(std::move(b));
            ++files_done_;
        }
        f.data.clear();
        // The first packet of a file carries a 10-byte transport header.
        if (plen > 12) f.data.insert(f.data.end(), pl + 10, pl + plen - 2);
        f.in_progress = true;
        if (seq == 3) {
            XritBlob b;
            b.vcid = (uint8_t)vcid;
            b.apid = apid;
            b.data = std::move(f.data);
            out.push_back(std::move(b));
            ++files_done_;
            f.in_progress = false;
        }
        return;
    }
    if (!f.in_progress) return;
    if (plen > 2) f.data.insert(f.data.end(), pl, pl + plen - 2);
    if (seq == 2) {
        XritBlob b;
        b.vcid = (uint8_t)vcid;
        b.apid = apid;
        b.data = std::move(f.data);
        out.push_back(std::move(b));
        ++files_done_;
        f.in_progress = false;
    }
}

}  // namespace eu
