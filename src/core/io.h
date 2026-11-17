// Everything normalises to complex<float> at full scale 1.0 (int16 /32768, int8 /128, cu8 (v-127.5)/128) so SNR is comparable across formats.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/common.h"

namespace eu {

enum class SampleFormat { Auto = 0, Cs16, Cs8, Cu8, Cf32, Ziq };

const char* fmt_name(SampleFormat f);
bool fmt_from_name(const std::string& s, SampleFormat& out);
SampleFormat fmt_from_path(const std::string& path);

struct SourceInfo {
    std::string   path;
    SampleFormat  fmt = SampleFormat::Auto;
    double        sample_rate = 0.0;   // Hz, 0 = unknown
    double        center_freq = 0.0;   // Hz, 0 = unknown
    int           bits = 0;            // bits per I/Q component
    uint64_t      total_samples = 0;   // 0 = unknown / streaming
    std::string   rate_source;         // "filename" | "header" | "cli"
};

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual size_t read(uint8_t* dst, size_t max) = 0;   // 0 = EOF
    virtual bool seek_bytes(uint64_t off) { (void)off; return false; }
};

class SampleSource {
public:
    virtual ~SampleSource() = default;
    virtual size_t read(cf* dst, size_t max) = 0;
    virtual bool   seek(uint64_t sample) { (void)sample; return false; }
    virtual bool   seekable() const { return false; }
    const SourceInfo& info() const { return info_; }
    SourceInfo& mutable_info() { return info_; }

    static std::unique_ptr<SampleSource> open(const std::string& path,
                                              SampleFormat want,
                                              double rate_override,
                                              double center_override,
                                              std::string* err);

protected:
    SourceInfo info_;
};

// Parses names like 2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16.
struct NameHints {
    bool     has_rate = false;
    bool     has_freq = false;
    double   rate = 0.0;
    double   freq = 0.0;
    std::string stem;
};
NameHints parse_name(const std::string& path);
// Extension and directory stripped, cropped to the timestamp stem.
std::string capture_stem(const std::string& path);

}  // namespace eu
