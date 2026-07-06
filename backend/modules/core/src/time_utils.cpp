#include "core/time_utils.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>

namespace argentum::core {

uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

uint64_t unix_now_ns() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void format_utc(uint64_t ts_ns, char* out, size_t out_len) {
    if (!out || out_len == 0) return;

    time_t secs = static_cast<time_t>(ts_ns / 1000000000ULL);
    uint32_t ns = static_cast<uint32_t>(ts_ns % 1000000000ULL);

    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);
    std::snprintf(out, out_len, "%s.%09u+00", base, ns);
}

std::string to_utc(uint64_t ts_ns) {
    char buffer[64];
    format_utc(ts_ns, buffer, sizeof(buffer));
    return std::string(buffer);
}

} // namespace argentum::core
