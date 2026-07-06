#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace argentum::core {

// Monotonic clock for latency measurements (not wall clock).
uint64_t now_ns();

// Wall clock (UTC) in nanoseconds since epoch.
uint64_t unix_now_ns();

// Format UTC timestamp to ISO-8601 with nanoseconds: YYYY-MM-DD HH:MM:SS.nnnnnnnnn+00
void format_utc(uint64_t ts_ns, char* out, size_t out_len);
std::string to_utc(uint64_t ts_ns);

} // namespace argentum::core
