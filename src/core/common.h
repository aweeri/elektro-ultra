// Shared basics: logging, timing, helpers.
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace eu {

using cf = std::complex<float>;
using cd = std::complex<double>;

constexpr double kPi = 3.14159265358979323846;

constexpr uint32_t kAsm = 0x1ACFFC1Du;
constexpr uint32_t kAsmInv = 0xE53003E2u;
constexpr int kCaduBits = 8192;
constexpr int kRsDepth = 4;
constexpr int kRsN = 255;
constexpr int kRsK = 223;
constexpr int kRsParity = 32;
constexpr int kCaduBytes = kCaduBits / 8;       // 1024
constexpr int kCaduInfoBytes = 4 + 4 * kRsK;    // 896
constexpr int kVcduHeaderBytes = 6;

constexpr double kLritBaud = 294000.0;
constexpr double kHritBaud = 1150000.0;
constexpr double kLritFreq = 1691.0e6;
constexpr double kHritFreq = 1691.0e6;
constexpr double kLritCaduRate = kLritBaud * 1.0 / (2.0 * kCaduBits);   //  17.9
constexpr double kHritCaduRate = kHritBaud * 2.0 / (2.0 * kCaduBits);   // 140.4

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

struct Log {
    static LogLevel level;
    static bool json;
    static FILE* out;
    static void write(LogLevel l, const char* fmt, ...);
};

#define EU_LOGE(...) ::eu::Log::write(::eu::LogLevel::Error, __VA_ARGS__)
#define EU_LOGW(...) ::eu::Log::write(::eu::LogLevel::Warn, __VA_ARGS__)
#define EU_LOGI(...) ::eu::Log::write(::eu::LogLevel::Info, __VA_ARGS__)
#define EU_LOGD(...) ::eu::Log::write(::eu::LogLevel::Debug, __VA_ARGS__)
#define EU_LOGT(...) ::eu::Log::write(::eu::LogLevel::Trace, __VA_ARGS__)

inline double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }
inline double dbp(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }
inline double wrap_pi(double x) {
    while (x > kPi) x -= 2.0 * kPi;
    while (x < -kPi) x += 2.0 * kPi;
    return x;
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double ms() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    void reset() { t0 = std::chrono::steady_clock::now(); }
};

inline int get_bit(const uint8_t* p, size_t i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
inline void put_bit(uint8_t* p, size_t i, int v) {
    if (v) p[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
    else   p[i >> 3] &= (uint8_t)~(1u << (7 - (i & 7)));
}

}  // namespace eu
