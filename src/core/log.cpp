#include "core/common.h"

#include <cstdarg>
#include <ctime>

namespace eu {

LogLevel Log::level = LogLevel::Info;
bool Log::json = false;
FILE* Log::out = stderr;

static const char* tag(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "ERR ";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Debug: return "DBG ";
        default:              return "TRC ";
    }
}

void Log::write(LogLevel l, const char* fmt, ...) {
    if ((int)l > (int)level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char ts[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    std::fprintf(out, "%s %s %s\n", ts, tag(l), buf);
    std::fflush(out);
}

}  // namespace eu
