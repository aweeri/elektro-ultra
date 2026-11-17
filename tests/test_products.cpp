// Product stage test: hand-built xRIT file through ProductWriter.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/common.h"
#include "xrit/products.h"
#include "xrit/transport.h"
#include "xrit/xrit.h"

using namespace eu;

static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
static void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, (uint32_t)(x >> 32));
    put32(v, (uint32_t)x);
}

static std::vector<uint8_t> make_xrit(const std::string& name, int cols, int lines,
                                      int bits, int compression,
                                      const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> hdr;
    hdr.push_back(0);
    put16(hdr, 16);
    hdr.push_back(0);                 // file type: image
    put32(hdr, 153);                  // total header length
    put64(hdr, payload.size());

    hdr.push_back(1);
    put16(hdr, 9);
    hdr.push_back((uint8_t)bits);
    put16(hdr, (uint16_t)cols);
    put16(hdr, (uint16_t)lines);
    hdr.push_back((uint8_t)compression);

    std::vector<uint8_t> ann(64, 0);
    std::memcpy(ann.data(), name.data(), std::min<size_t>(name.size(), 63));
    hdr.push_back(4);
    put16(hdr, (uint16_t)(ann.size() + 3));
    hdr.insert(hdr.end(), ann.begin(), ann.end());

    hdr.resize(153, 0);
    hdr.insert(hdr.end(), payload.begin(), payload.end());
    return hdr;
}

int main(int argc, char** argv) {
    const std::string out = argc > 2 ? argv[2] : "/tmp/eu_products";
    mkdir(out.c_str(), 0755);

    ProductWriter pw;
    ProductStats st;
    pw.configure(out, "LRIT", true);

    const int C = 96, R = 64;
    std::vector<uint8_t> raw((size_t)C * R);
    for (int y = 0; y < R; ++y)
        for (int x = 0; x < C; ++x)
            raw[(size_t)y * C + x] = (uint8_t)((x * 2 + y * 3) & 0xFF);
    auto file = make_xrit("L-000-TEST_-TEST_4______-01_8_076E-000001___-202601010000-C_",
                          C, R, 8, 0, raw);

    XritBlob b;
    b.data = file;
    pw.push(b, st);
    pw.flush(st);
    std::printf("raw: files %llu images %llu pngs %llu raws %llu\n",
                (unsigned long long)st.files_seen, (unsigned long long)st.images,
                (unsigned long long)st.pngs, (unsigned long long)st.raws);

    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "rb");
        if (f) {
            std::vector<uint8_t> jpg;
            uint8_t buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                jpg.insert(jpg.end(), buf, buf + n);
            std::fclose(f);
            auto jf = make_xrit("L-000-TEST_-TEST_4______-01_8_076E-000002___-202601010000-C_",
                                C, R, 8, 2, jpg);
            XritBlob jb;
            jb.data = jf;
            ProductStats st2;
            pw.push(jb, st2);
            pw.flush(st2);
            std::printf("jpeg: images %llu pngs %llu jpeg_fail %llu\n",
                        (unsigned long long)st2.images, (unsigned long long)st2.pngs,
                        (unsigned long long)st2.jpeg_fail);
        }
    }
    return 0;
}
