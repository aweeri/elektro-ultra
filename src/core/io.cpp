#include "core/io.h"

#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>

namespace eu {

const char* fmt_name(SampleFormat f) {
    switch (f) {
        case SampleFormat::Cs16: return "cs16";
        case SampleFormat::Cs8:  return "cs8";
        case SampleFormat::Cu8:  return "cu8";
        case SampleFormat::Cf32: return "cf32";
        case SampleFormat::Ziq:  return "ziq";
        default:                 return "auto";
    }
}

bool fmt_from_name(const std::string& s, SampleFormat& out) {
    std::string t;
    for (char c : s) t.push_back((char)std::tolower((unsigned char)c));
    if (t == "cs16" || t == "int16" || t == "s16") { out = SampleFormat::Cs16; return true; }
    if (t == "cs8"  || t == "int8"  || t == "s8")  { out = SampleFormat::Cs8;  return true; }
    if (t == "cu8"  || t == "uint8" || t == "u8")  { out = SampleFormat::Cu8;  return true; }
    if (t == "cf32" || t == "f32"   || t == "fc32"){ out = SampleFormat::Cf32; return true; }
    if (t == "ziq")                                { out = SampleFormat::Ziq;  return true; }
    if (t == "auto")                               { out = SampleFormat::Auto; return true; }
    return false;
}

SampleFormat fmt_from_path(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return SampleFormat::Cs16;
    std::string ext = path.substr(dot + 1);
    SampleFormat f;
    if (fmt_from_name(ext, f)) return f;
    return SampleFormat::Cs16;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

NameHints parse_name(const std::string& path) {
    NameHints h;
    std::string base = path;
    const size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    h.stem = base;

    // _<digits>SPS_
    size_t p = base.find("SPS");
    if (p != std::string::npos) {
        size_t q = p;
        while (q > 0 && std::isdigit((unsigned char)base[q - 1])) --q;
        if (q < p) {
            const std::string num = base.substr(q, p - q);
            if (all_digits(num)) { h.rate = std::stod(num); h.has_rate = true; }
        }
    }
    // _<digits>Hz
    size_t hz = base.rfind("Hz");
    if (hz != std::string::npos) {
        size_t q = hz;
        while (q > 0 && std::isdigit((unsigned char)base[q - 1])) --q;
        if (q < hz) {
            const std::string num = base.substr(q, hz - q);
            if (all_digits(num)) { h.freq = std::stod(num); h.has_freq = true; }
        }
    }
    return h;
}

std::string capture_stem(const std::string& path) {
    NameHints h = parse_name(path);
    // Prefer the timestamp part: YYYY-MM-DD_HH-MM-SS
    const std::string& s = h.stem;
    if (s.size() >= 19 && s[4] == '-' && s[7] == '-' && s[10] == '_' && s[13] == '-' && s[16] == '-')
        return s.substr(0, 19);
    return s;
}

class FileBytes : public ByteSource {
public:
    explicit FileBytes(const std::string& path) {
        f_ = std::fopen(path.c_str(), "rb");
    }
    ~FileBytes() override { if (f_) std::fclose(f_); }
    bool ok() const { return f_ != nullptr; }
    size_t read(uint8_t* dst, size_t max) override {
        if (!f_) return 0;
        return std::fread(dst, 1, max, f_);
    }
    bool seek_bytes(uint64_t off) override {
        if (!f_) return false;
        return std::fseek(f_, (long)off, SEEK_SET) == 0;
    }
    uint64_t size() const {
        if (!f_) return 0;
        const long cur = std::ftell(f_);
        std::fseek(f_, 0, SEEK_END);
        const long n = std::ftell(f_);
        std::fseek(f_, cur, SEEK_SET);
        return n > 0 ? (uint64_t)n : 0;
    }
private:
    FILE* f_ = nullptr;
};

// ziq: 22-byte header + zstd stream, resolved at runtime from libzstd.so.1.
namespace zstd {

typedef struct ZSTD_DCtx_s ZSTD_DStream;
struct InBuffer  { const void* src; size_t size; size_t pos; };
struct OutBuffer { void* dst; size_t size; size_t pos; };

struct Api {
    ZSTD_DStream* (*createDStream)();
    size_t        (*initDStream)(ZSTD_DStream*);
    size_t        (*decompressStream)(ZSTD_DStream*, OutBuffer*, InBuffer*);
    unsigned      (*isError)(size_t);
    const char*   (*getErrorName)(size_t);
    size_t        (*freeDStream)(ZSTD_DStream*);
    bool ok = false;
};

inline const Api& api() {
    static Api a{};
    static bool tried = false;
    if (!tried) {
        tried = true;
        void* h = dlopen("libzstd.so.1", RTLD_LAZY);
        if (!h) h = dlopen("libzstd.so", RTLD_LAZY);
        if (h) {
            a.createDStream     = (decltype(a.createDStream))dlsym(h, "ZSTD_createDStream");
            a.initDStream       = (decltype(a.initDStream))dlsym(h, "ZSTD_initDStream");
            a.decompressStream  = (decltype(a.decompressStream))dlsym(h, "ZSTD_decompressStream");
            a.isError           = (decltype(a.isError))dlsym(h, "ZSTD_isError");
            a.getErrorName      = (decltype(a.getErrorName))dlsym(h, "ZSTD_getErrorName");
            a.freeDStream       = (decltype(a.freeDStream))dlsym(h, "ZSTD_freeDStream");
            a.ok = a.createDStream && a.initDStream && a.decompressStream &&
                   a.isError && a.getErrorName;
        }
    }
    return a;
}

}  // namespace zstd

struct ZiqHeader {
    uint8_t  version = 0;
    uint8_t  bits = 8;
    uint32_t sample_rate = 0;
    uint64_t center_freq = 0;
    bool     parsed = false;
};

inline ZiqHeader parse_ziq_header(const uint8_t h[22]) {
    ZiqHeader z;
    if (!(h[0] == 'Z' && h[1] == 'I' && h[2] == 'Q' && h[3] == '_')) return z;
    z.version = h[4];
    z.bits = h[5];
    std::memcpy(&z.sample_rate, h + 6, 4);
    std::memcpy(&z.center_freq, h + 10, 8);
    z.parsed = true;
    return z;
}

class ZstdBytes : public ByteSource {
public:
    explicit ZstdBytes(const std::string& path) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) return;
        uint8_t hdr[22];
        if (std::fread(hdr, 1, 22, f_) != 22) return;
        hdr_ = parse_ziq_header(hdr);
        if (!hdr_.parsed) { std::fclose(f_); f_ = nullptr; return; }
        const zstd::Api& a = zstd::api();
        if (!a.ok) return;
        ds_ = a.createDStream();
        if (!ds_) { std::fclose(f_); f_ = nullptr; return; }
        a.initDStream(ds_);
        in_.resize(1 << 20);
        ok_ = true;
    }
    ~ZstdBytes() override {
        const zstd::Api& a = zstd::api();
        if (ds_ && a.ok && a.freeDStream) a.freeDStream(ds_);
        if (f_) std::fclose(f_);
    }
    bool ok() const { return ok_; }
    const ZiqHeader& header() const { return hdr_; }

    size_t read(uint8_t* dst, size_t max) override {
        if (!ok_) return 0;
        const zstd::Api& a = zstd::api();
        size_t produced = 0;
        while (produced < max) {
            if (in_pos_ == in_len_) {
                in_len_ = std::fread(in_.data(), 1, in_.size(), f_);
                in_pos_ = 0;
                if (in_len_ == 0) break;
            }
            zstd::InBuffer ib{in_.data() + in_pos_, in_len_ - in_pos_, 0};
            zstd::OutBuffer ob{dst + produced, max - produced, 0};
            const size_t r = a.decompressStream(ds_, &ob, &ib);
            in_pos_ += ib.pos;
            produced += ob.pos;
            if (a.isError(r)) {
                EU_LOGE("zstd: %s", a.getErrorName(r));
                ok_ = false;
                break;
            }
            if (ob.pos == 0 && ib.pos == 0 && r != 0) break;   // need input
            if (r == 0) { a.initDStream(ds_); }                // frame done
        }
        return produced;
    }

private:
    FILE* f_ = nullptr;
    zstd::ZSTD_DStream* ds_ = nullptr;
    ZiqHeader hdr_{};
    std::vector<uint8_t> in_;
    size_t in_len_ = 0, in_pos_ = 0;
    bool ok_ = false;
};

class ByteSampleSource : public SampleSource {
public:
    ByteSampleSource(const std::string& path, SampleFormat fmt,
                     std::unique_ptr<ByteSource> bytes, size_t bytes_per_complex,
                     uint64_t total_bytes)
        : fmt_(fmt), bytes_(std::move(bytes)), bpc_(bytes_per_complex) {
        info_.path = path;
        info_.fmt = fmt;
        if (total_bytes && bpc_) info_.total_samples = total_bytes / bpc_;
        buf_.resize(1 << 20);
    }

    size_t read(cf* dst, size_t max) override {
        const size_t need = max * bpc_;
        if (buf_.size() < need) buf_.resize(need);
        size_t got = 0;
        while (got < need) {
            const size_t n = bytes_->read(buf_.data() + got, need - got);
            if (n == 0) break;
            got += n;
        }
        const size_t aligned = (got / bpc_) * bpc_;
        const size_t nsamp = aligned / bpc_;
        const uint8_t* p = buf_.data();
        switch (fmt_) {
            case SampleFormat::Cs16: {
                const int16_t* s = reinterpret_cast<const int16_t*>(p);
                for (size_t i = 0; i < nsamp; ++i)
                    dst[i] = cf(s[2 * i] * (1.0f / 32768.0f), s[2 * i + 1] * (1.0f / 32768.0f));
                break;
            }
            case SampleFormat::Cs8: {
                const int8_t* s = reinterpret_cast<const int8_t*>(p);
                for (size_t i = 0; i < nsamp; ++i)
                    dst[i] = cf(s[2 * i] * (1.0f / 128.0f), s[2 * i + 1] * (1.0f / 128.0f));
                break;
            }
            case SampleFormat::Cu8: {
                for (size_t i = 0; i < nsamp; ++i)
                    dst[i] = cf((p[2 * i] - 127.5f) * (1.0f / 128.0f),
                                (p[2 * i + 1] - 127.5f) * (1.0f / 128.0f));
                break;
            }
            case SampleFormat::Cf32: {
                std::memcpy(dst, p, nsamp * sizeof(cf));
                break;
            }
            default:
                return 0;
        }
        pos_ += nsamp;
        return nsamp;
    }

    bool seek(uint64_t sample) override {
        if (!bytes_) return false;
        if (!bytes_->seek_bytes(sample * bpc_)) return false;
        pos_ = sample;
        return true;
    }
    bool seekable() const override { return fmt_ != SampleFormat::Ziq; }

private:
    SampleFormat fmt_;
    std::unique_ptr<ByteSource> bytes_;
    size_t bpc_ = 4;
    std::vector<uint8_t> buf_;
    uint64_t pos_ = 0;
};

std::unique_ptr<SampleSource> SampleSource::open(const std::string& path,
                                                 SampleFormat want,
                                                 double rate_override,
                                                 double center_override,
                                                 std::string* err) {
    auto fail = [&](const std::string& m) -> std::unique_ptr<SampleSource> {
        if (err) *err = m;
        return nullptr;
    };
    const NameHints hints = parse_name(path);
    SampleFormat fmt = (want == SampleFormat::Auto) ? fmt_from_path(path) : want;

    if (fmt == SampleFormat::Ziq) {
        auto z = std::make_unique<ZstdBytes>(path);
        if (!z->ok()) return fail("cannot open ziq (header or libzstd.so.1 missing): " + path);
        const ZiqHeader h = z->header();
        const int bits = h.bits ? h.bits : 8;
        const size_t bpc = (size_t)(2 * (bits / 8));
        auto src = std::make_unique<ByteSampleSource>(path,
                        bits == 16 ? SampleFormat::Cs16 : SampleFormat::Cs8,
                        std::move(z), bpc, 0);
        src->mutable_info().fmt = SampleFormat::Ziq;
        src->mutable_info().bits = bits;
        src->mutable_info().sample_rate = rate_override > 0 ? rate_override : (double)h.sample_rate;
        src->mutable_info().rate_source = rate_override > 0 ? "cli" : "header";
        src->mutable_info().center_freq = center_override > 0 ? center_override : (double)h.center_freq;
        return src;
    }

    auto f = std::make_unique<FileBytes>(path);
    if (!f->ok()) return fail("cannot open " + path);
    const uint64_t size = f->size();
    size_t bpc = 4;
    int bits = 16;
    switch (fmt) {
        case SampleFormat::Cs16: bpc = 4; bits = 16; break;
        case SampleFormat::Cs8:  bpc = 2; bits = 8;  break;
        case SampleFormat::Cu8:  bpc = 2; bits = 8;  break;
        case SampleFormat::Cf32: bpc = 8; bits = 32; break;
        default: break;
    }
    auto src = std::make_unique<ByteSampleSource>(path, fmt, std::move(f), bpc, size);
    src->mutable_info().bits = bits;
    if (hints.has_rate) { src->mutable_info().sample_rate = hints.rate; src->mutable_info().rate_source = "filename"; }
    if (rate_override > 0) { src->mutable_info().sample_rate = rate_override; src->mutable_info().rate_source = "cli"; }
    if (hints.has_freq) src->mutable_info().center_freq = hints.freq;
    if (center_override > 0) src->mutable_info().center_freq = center_override;
    return src;
}

}  // namespace eu
