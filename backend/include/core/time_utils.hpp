#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace argentum::core {

// Single source of time for the whole codebase (ADR 0014). Two explicit
// clock domains, never mixed:
//
//   mono_now_ns()  — monotonic clock. DURATIONS ONLY. Never persisted as an
//                    event time; survives no meaning across processes.
//   wall_now_ns()  — UTC wall clock, nanoseconds since the Unix epoch.
//                    EVENT STAMPS ONLY. Subject to NTP steps/slew, so never
//                    used to measure elapsed time.
//
// Direct std::chrono clock calls outside modules/core/src/time_utils.cpp are
// banned and enforced by scripts/check_clock_discipline.py in CI.

/// Monotonic nanoseconds. Durations only.
uint64_t mono_now_ns();

/// UTC wall-clock nanoseconds since epoch. Event stamps only.
uint64_t wall_now_ns();

/// Deprecated aliases (pre-ADR-0014 names). New code uses the explicit names.
inline uint64_t now_ns() {
    return mono_now_ns();
}
inline uint64_t unix_now_ns() {
    return wall_now_ns();
}

// Format UTC timestamp to ISO-8601 with nanoseconds: YYYY-MM-DD HH:MM:SS.nnnnnnnnn+00
void format_utc(uint64_t ts_ns, char* out, size_t out_len);
std::string to_utc(uint64_t ts_ns);

/**
 * @brief Measured mapping between the monotonic and wall clock domains.
 *
 * Sampled as wall-clock bracketed by two monotonic reads; the pair with the
 * narrowest bracket wins and half that bracket is the mapping uncertainty.
 * This is what lets a monotonic span be projected onto wall time with a known
 * error bound — the precondition for cross-referencing internal latency spans
 * against venue/exchange timestamps (markouts, Block 2).
 *
 * The mapping drifts as NTP disciplines the wall clock: re-measure
 * periodically OFF the hot path and compare `wall_at()` projections to detect
 * drift (see argentum_clock_check).
 */
struct ClockCalibration {
    uint64_t mono_ns = 0;         // monotonic anchor
    uint64_t wall_ns = 0;         // wall-clock anchor observed at the monotonic anchor
    uint64_t uncertainty_ns = 0;  // half of the narrowest sampling bracket

    /// Takes `samples` bracketed readings and keeps the tightest. Not for hot paths.
    static ClockCalibration measure(int samples = 9);

    [[nodiscard]] bool valid() const {
        return mono_ns != 0 && wall_ns != 0;
    }

    /// Projects a monotonic instant onto the wall clock using this mapping.
    [[nodiscard]] uint64_t wall_at(uint64_t mono_instant_ns) const {
        if (!valid()) return 0;
        if (mono_instant_ns >= mono_ns) {
            return wall_ns + (mono_instant_ns - mono_ns);
        }
        const uint64_t back = mono_ns - mono_instant_ns;
        return (back >= wall_ns) ? 0 : (wall_ns - back);
    }
};

} // namespace argentum::core
