#include "xrit/products.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/common.h"

#include "CompressWT.h"
#include "CDataField.h"
#include "stb_image.h"
#include "xrit/png.h"

namespace eu {

void ProductWriter::configure(const std::string& out_root, const std::string& type,
                              bool write_images) {
    root_ = out_root;
    type_ = type;
    images_ = write_images;
    accs_.clear();
    seq_ = 0;
}

std::string ProductWriter::safe_name(const std::string& s) const {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.')
            o.push_back(c);
        else
            o.push_back('_');
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    if (o.empty()) o = "xrit";
    return o;
}

void ProductWriter::push(const XritBlob& blob, ProductStats& st) {
    if (blob.data.size() < 16) return;

    XritHeaders h;
    if (!parse_xrit_headers(blob.data, h) || h.total_header_length >= blob.data.size())
        return;

    std::string key = h.annotation;
    if (key.empty()) key = "apid" + std::to_string(blob.apid);

    Acc& a = accs_[key];
    if (!a.started) {
        a.started = true;
        a.h = h;
        a.want = (uint64_t)h.total_header_length + h.data_field_length;
        ++st.files_seen;
    }

    // The declared data field length runs far ahead of what one pass carries, so completeness is decided from the payload.
    if (!a.data.empty() && looks_complete(a)) {
        finish(a, st);
        ++st.files_complete;
        st.files_seen++;
        a.data.clear();
        a.h = h;
        a.want = (uint64_t)h.total_header_length + h.data_field_length;
    }

    if (a.data.empty()) {
        a.data.insert(a.data.end(), blob.data.begin(), blob.data.end());
    } else {
        // The headers repeat on every segment; the payload does not.
        const size_t skip = std::min<size_t>(h.total_header_length, blob.data.size());
        a.data.insert(a.data.end(), blob.data.begin() + (long)skip, blob.data.end());
    }
    a.last_seen = ++seq_;
    st.bytes += blob.data.size();

    if (looks_complete(a)) {
        finish(a, st);
        ++st.files_complete;
        a.data.clear();
        a.started = false;
    }
}

bool ProductWriter::looks_complete(const Acc& a) const {
    if (a.data.size() < 16) return false;
    const size_t thl = a.h.total_header_length;
    if (thl >= a.data.size()) return false;
    const uint8_t* payload = a.data.data() + thl;
    const size_t len = a.data.size() - thl;

    if (a.h.file_type_code == 0 && a.h.has_image) {
        if (a.h.compression == 0) {
            const uint64_t need =
                (uint64_t)a.h.columns * a.h.lines * ((a.h.bits_per_pixel + 7) / 8);
            return len >= need;
        }
        // EOI is unambiguous: entropy data stuffs 0xFF as FF 00.
        if (a.h.compression == 2) {
            for (size_t i = 0; i + 1 < len; ++i)
                if (payload[i] == 0xFF && payload[i + 1] == 0xD9) return true;
        }
    }
    return a.want && a.data.size() >= a.want;
}

void ProductWriter::flush(ProductStats& st) {
    for (auto& kv : accs_)
        if (kv.second.started && kv.second.data.size() > kv.second.h.total_header_length) {
            // Emit a short image rather than hide it.
            finish(kv.second, st);
        }
    accs_.clear();
    for (auto& kv : segs_) stitch(kv.first, kv.second, st);
    segs_.clear();
}

void ProductWriter::finish(Acc& a, ProductStats& st) {
    if (a.data.size() < 16) return;
    XritHeaders h = a.h;
    if (!h.valid) parse_xrit_headers(a.data, h);
    if (!h.valid) return;

    const std::string stem = safe_name(h.annotation.empty()
                                           ? ("apid" + std::to_string(0))
                                           : h.annotation);

    if (h.file_type_code != 0 || !h.has_image) {
        // Not an image: keep the bytes.
        ++st.raws;
        if (!images_ || root_.empty()) return;
        const std::string dir = root_ + "/raw";
        ::mkdir(root_.c_str(), 0755);
        ::mkdir(dir.c_str(), 0755);
        const std::string path = dir + "/" + stem + ".bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(a.data.data(), 1, a.data.size(), f);
            std::fclose(f);
        }
        return;
    }

    ++st.images;
    const uint8_t* payload = a.data.data() + h.total_header_length;
    const size_t payload_len = a.data.size() - h.total_header_length;

    std::vector<uint8_t> gray8;
    std::vector<uint16_t> gray16;
    int cols = h.columns, rows = h.lines;

    if (h.compression == 2) {
        int w = 0, ht = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(payload, (int)payload_len, &w, &ht, &comp, 1);
        if (!px) {
            ++st.jpeg_fail;
            EU_LOGW("products: JPEG decode failed for %s (%zu bytes)", stem.c_str(),
                    payload_len);
            if (!root_.empty()) {
                const std::string dir = root_ + "/raw";
                ::mkdir(root_.c_str(), 0755);
                ::mkdir(dir.c_str(), 0755);
                if (FILE* f = std::fopen((dir + "/" + stem + ".jpg").c_str(), "wb")) {
                    std::fwrite(payload, 1, payload_len, f);
                    std::fclose(f);
                }
            }
            return;
        }
        cols = w;
        rows = ht;
        gray8.assign(px, px + (size_t)w * ht);
        stbi_image_free(px);
    } else if (h.compression == 0) {
        // Wider than 8 bit is big-endian.
        if (h.bits_per_pixel <= 8) {
            if (payload_len < (size_t)cols * rows) return;
            gray8.assign(payload, payload + (size_t)cols * rows);
        } else {
            if (payload_len < (size_t)cols * rows * 2) return;
            gray16.resize((size_t)cols * rows);
            for (size_t i = 0; i < gray16.size(); ++i)
                gray16[i] = (uint16_t)((payload[i * 2] << 8) | payload[i * 2 + 1]);
        }
    } else if (h.compression == 1) {
        // DecompWT returns the image packed at the declared depth (10 bit = 1.25 bytes/pixel) so it needs unpacking; the buffer must be malloc'd, the data field takes ownership.
        uint8_t* cbuf = (uint8_t*)std::malloc(payload_len ? payload_len : 1);
        if (!cbuf) return;
        std::memcpy(cbuf, payload, payload_len);
        Util::CDataFieldCompressedImage cimg(cbuf, (unsigned __int64)payload_len * 8,
                                             (unsigned char)h.bits_per_pixel,
                                             (unsigned short)h.columns,
                                             (unsigned short)h.lines);
        Util::CDataFieldUncompressedImage dimg;
        std::vector<short> quality;
        bool ok = true;
        try {
            COMP::DecompressWT(cimg, (unsigned char)h.bits_per_pixel, dimg, quality);
        } catch (const std::exception& e) {
            ok = false;
            EU_LOGW("products: wavelet decode failed for %s (%s)", stem.c_str(), e.what());
        }
        const size_t npix = (size_t)h.columns * (size_t)h.lines;
        const size_t nbits = npix * (size_t)h.bits_per_pixel;
        if (ok && h.bits_per_pixel > 0 && dimg.Size() >= (unsigned __int64)nbits) {
            std::vector<uint8_t> packed((nbits + 7) / 8);
            dimg.Read(0, packed.data(), (int)packed.size());
            gray16.resize(npix);
            size_t bit = 0;
            for (size_t i = 0; i < npix; ++i) {
                unsigned v = 0;
                for (int b = 0; b < h.bits_per_pixel; ++b, ++bit)
                    v = (v << 1) | ((packed[bit >> 3] >> (7 - (bit & 7))) & 1u);
                gray16[i] = (uint16_t)v;
            }
            cols = h.columns;
            rows = h.lines;
        } else {
            ++st.unsupported;
            EU_LOGW("products: %s wavelet payload did not decode [%ux%u, %u bits, %zu bytes]",
                    stem.c_str(), (unsigned)h.columns, (unsigned)h.lines,
                    (unsigned)h.bits_per_pixel, payload_len);
            if (!root_.empty()) {
                const std::string dir = root_ + "/raw";
                ::mkdir(root_.c_str(), 0755);
                ::mkdir(dir.c_str(), 0755);
                if (FILE* f = std::fopen((dir + "/" + stem + ".wt").c_str(), "wb")) {
                    std::fwrite(payload, 1, payload_len, f);
                    std::fclose(f);
                }
            }
            return;
        }
    } else {
        ++st.unsupported;
        EU_LOGW("products: %s uses compression %d, no decoder", stem.c_str(), h.compression);
        return;
    }

    if (!images_ || root_.empty()) return;

    if (!gray8.empty()) add_segment(h, cols, rows, gray8, st);
    if (!gray16.empty() && h.compression == 1) add_segment16(h, cols, rows, gray16, st);

    const std::string dir = root_ + "/" + type_;
    ::mkdir(root_.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);

    const std::string path = dir + "/" + stem + ".png";
    bool ok = false;
    if (!gray16.empty()) ok = png_write_gray16(path, cols, rows, gray16);
    else ok = png_write_gray8(path, cols, rows, gray8.data());
    if (ok) ++st.pngs;
    else EU_LOGW("products: cannot write %s", path.c_str());
}

// Product name with the segment field blanked, so all strips share a bucket.
static std::string stitch_key(const std::string& name) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '-') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    if (parts.size() >= 6) parts[5] = "SEG";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '-';
        out += parts[i];
    }
    return out;
}

void ProductWriter::add_segment(const XritHeaders& h, int cols, int rows,
                                const std::vector<uint8_t>& g8, ProductStats& st) {
    (void)st;
    if (!images_ || root_.empty() || h.segment <= 0 || g8.empty()) return;
    if ((size_t)cols * (size_t)rows != g8.size()) return;
    const std::string key = stitch_key(h.annotation);
    SegSet& s = segs_[key];
    if (s.segs.empty()) { s.cols = cols; s.rows = rows; s.bits = 8; }
    if (s.cols != cols || s.rows != rows || s.bits != 8) return;
    std::vector<uint16_t> wide(g8.begin(), g8.end());
    s.segs[h.segment] = std::move(wide);
}

void ProductWriter::add_segment16(const XritHeaders& h, int cols, int rows,
                                  const std::vector<uint16_t>& g16, ProductStats& st) {
    (void)st;
    if (!images_ || root_.empty() || h.segment <= 0 || g16.empty()) return;
    if ((size_t)cols * (size_t)rows != g16.size()) return;
    const std::string key = stitch_key(h.annotation);
    SegSet& s = segs_[key];
    if (s.segs.empty()) { s.cols = cols; s.rows = rows; s.bits = 16; }
    if (s.cols != cols || s.rows != rows || s.bits != 16) return;
    s.segs[h.segment] = g16;
}

void ProductWriter::stitch(const std::string& key, SegSet& s, ProductStats& st) {
    if (s.segs.size() < 2) return;
    const int lo = s.segs.begin()->first;
    const int hi = s.segs.rbegin()->first;
    if ((int)s.segs.size() != hi - lo + 1) {
        EU_LOGW("products: %s: segments %d..%d, only %zu present - not stitching",
                key.c_str(), lo, hi, s.segs.size());
        return;
    }
    const int n = hi - lo + 1;
    const int W = s.cols, H = s.rows * n;
    std::vector<uint16_t> out((size_t)W * (size_t)H, 0);
    for (int i = 0; i < n; ++i) {
        const auto it = s.segs.find(lo + i);
        std::memcpy(out.data() + (size_t)i * (size_t)s.rows * (size_t)W,
                    it->second.data(), (size_t)s.rows * (size_t)W * sizeof(uint16_t));
    }

    // 0.5/99.5 percentile stretch for display only; strips stay raw.
    const int top = s.bits > 8 ? 1023 : 255;
    std::vector<uint64_t> hist((size_t)top + 1, 0);
    for (uint16_t v : out) hist[v > top ? top : v]++;
    const double total = (double)out.size();
    auto pct = [&](double q) {
        double acc = 0.0;
        for (int v = 0; v <= top; ++v) {
            acc += (double)hist[v];
            if (acc >= q * total) return v;
        }
        return top;
    };
    const int plo = pct(0.005), phi = pct(0.995);

    const std::string dir = root_ + "/full";
    ::mkdir(root_.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/" + safe_name(key) + ".png";

    bool ok = false;
    if (s.bits > 8) {
        if (phi - plo >= 32) {
            const double scale = (double)top / (double)(phi - plo);
            for (auto& v : out) {
                const double x = ((double)v - plo) * scale;
                v = (uint16_t)(x < 0.0 ? 0.0 : (x > top ? top : x));
            }
        }
        ok = png_write_gray16(path, W, H, out);
    } else {
        std::vector<uint8_t> narrow(out.size());
        const double scale = (phi - plo >= 8) ? 255.0 / (double)(phi - plo) : 1.0;
        for (size_t i = 0; i < out.size(); ++i) {
            const double x = ((double)out[i] - plo) * scale;
            narrow[i] = (uint8_t)(x < 0.0 ? 0.0 : (x > 255.0 ? 255.0 : x));
        }
        ok = png_write_gray8(path, W, H, narrow.data());
    }
    if (ok) ++st.stitched;
    else EU_LOGW("products: cannot write %s", path.c_str());
}

}  // namespace eu
