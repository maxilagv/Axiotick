#include "core/time_utils.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>

namespace argentum::core {

// The ONLY translation unit allowed to touch std::chrono clocks directly
// (enforced by scripts/check_clock_discipline.py). steady_clock maps to
// QueryPerformanceCounter on Windows and CLOCK_MONOTONIC on POSIX.

uint64_t mono_now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

uint64_t wall_now_ns() {
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

ClockCalibration ClockCalibration::measure(int samples) {
    if (samples < 1) samples = 1;

    ClockCalibration best{};
    uint64_t best_bracket = UINT64_MAX;

    for (int i = 0; i < samples; ++i) {
        const uint64_t mono_before = mono_now_ns();
        const uint64_t wall = wall_now_ns();
        const uint64_t mono_after = mono_now_ns();
        if (mono_after < mono_before) continue;  // scheduler artifact; discard

        const uint64_t bracket = mono_after - mono_before;
        if (bracket < best_bracket) {
            best_bracket = bracket;
            best.mono_ns = mono_before + bracket / 2;
            best.wall_ns = wall;
            best.uncertainty_ns = bracket / 2;
        }
    }

    return best;
}

} // namespace argentum::core
