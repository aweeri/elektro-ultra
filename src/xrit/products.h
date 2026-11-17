// A file is spread over many passes, each restarting the headers and carrying a slice of the data field, so reassembly keys on the product filename.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xrit/transport.h"
#include "xrit/xrit.h"

namespace eu {

struct ProductStats {
    uint64_t files_seen = 0;
    uint64_t files_complete = 0;
    uint64_t images = 0;
    uint64_t pngs = 0;
    uint64_t stitched = 0;
    uint64_t raws = 0;
    uint64_t unsupported = 0;
    uint64_t jpeg_fail = 0;
    uint64_t bytes = 0;
};

class ProductWriter {
public:
    void configure(const std::string& out_root, const std::string& type, bool write_images);
    void push(const XritBlob& blob, ProductStats& st);
    void flush(ProductStats& st);

private:
    struct Acc {
        std::vector<uint8_t> data;
        XritHeaders h;
        uint64_t want = 0;
        bool started = false;
        uint64_t last_seen = 0;
    };

    void finish(Acc& a, ProductStats& st);
    bool looks_complete(const Acc& a) const;
    std::string safe_name(const std::string& s) const;

    // Strips are one xRIT file each; stack them once the run is contiguous.
    struct SegSet {
        int cols = 0, rows = 0;
        int bits = 8;      // HRIT wavelet strips are 10-bit
        std::map<int, std::vector<uint16_t>> segs;
    };
    void add_segment(const XritHeaders& h, int cols, int rows,
                     const std::vector<uint8_t>& g8, ProductStats& st);
    void add_segment16(const XritHeaders& h, int cols, int rows,
                       const std::vector<uint16_t>& g16, ProductStats& st);
    void stitch(const std::string& key, SegSet& s, ProductStats& st);

    std::string root_;
    std::string type_;
    bool images_ = true;
    std::map<std::string, Acc> accs_;
    std::map<std::string, SegSet> segs_;
    uint64_t seq_ = 0;
};

}  // namespace eu
