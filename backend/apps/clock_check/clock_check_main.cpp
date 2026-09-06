// Clock discipline check (ADR 0014, capability C of the differentiation
// thesis): measures the mono<->wall mapping repeatedly and reports offset
// drift. Without a disciplined wall clock, sub-second markouts and any
// cross-host latency comparison are noise — this tool is how a run PROVES
// what its clock error bound was.
//
// Usage: argentum_clock_check [rounds] [interval_ms] [json_path]
// Defaults: 5 rounds, 1000 ms apart, no JSON.

#include "core/time_utils.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const int rounds = (argc > 1) ? std::max(2, std::atoi(argv[1])) : 5;
    const int interval_ms = (argc > 2) ? std::max(10, std::atoi(argv[2])) : 1000;
    const std::string json_path = (argc > 3) ? argv[3] : "";

    std::printf("=== Axiotick clock check ===\n");
    std::printf("rounds=%d interval=%dms\n\n", rounds, interval_ms);

    using argentum::core::ClockCalibration;

    std::vector<ClockCalibration> calibrations;
    calibrations.reserve(static_cast<size_t>(rounds));

    for (int i = 0; i < rounds; ++i) {
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        const ClockCalibration cal = ClockCalibration::measure(15);
        calibrations.push_back(cal);
        std::printf("round %d: mono=%" PRIu64 " wall=%" PRIu64 " uncertainty=%" PRIu64 "ns\n",
                    i + 1, cal.mono_ns, cal.wall_ns, cal.uncertainty_ns);
    }

    // Drift: how far the first calibration's projection is from reality by
    // the last round. A steady, NTP-disciplined host stays in the low
    // microseconds over seconds; a slewing/stepping clock will show here.
    const ClockCalibration& first = calibrations.front();
    const ClockCalibration& last = calibrations.back();

    const uint64_t projected_wall = first.wall_at(last.mono_ns);
    const int64_t drift_ns = static_cast<int64_t>(last.wall_ns) - static_cast<int64_t>(projected_wall);
    const uint64_t elapsed_mono_ns = last.mono_ns - first.mono_ns;
    const double elapsed_s = static_cast<double>(elapsed_mono_ns) / 1e9;
    const double drift_ppm =
        (elapsed_mono_ns == 0) ? 0.0 : (static_cast<double>(drift_ns) / static_cast<double>(elapsed_mono_ns)) * 1e6;

    uint64_t worst_uncertainty = 0;
    for (const ClockCalibration& cal : calibrations) {
        if (cal.uncertainty_ns > worst_uncertainty) worst_uncertainty = cal.uncertainty_ns;
    }

    std::printf("\n--- verdict ---\n");
    std::printf("elapsed              : %.3f s (mono)\n", elapsed_s);
    std::printf("wall-vs-mono drift   : %lld ns (%.3f ppm)\n",
                static_cast<long long>(drift_ns), drift_ppm);
    std::printf("worst calibration    : +/-%" PRIu64 " ns\n", worst_uncertainty);
    std::printf("current error bound  : |drift| + uncertainty = %" PRIu64 " ns\n",
                static_cast<uint64_t>((drift_ns < 0 ? -drift_ns : drift_ns)) + worst_uncertainty);
    std::printf("\nLive-readiness requirement: NTP-disciplined wall clock with measured\n"
                "offset < 1 ms (see docs/LIVE_TRADING_READINESS_CHECKLIST.md).\n"
                "Windows: w32tm /query /status   Linux: chronyc tracking\n");

    if (!json_path.empty()) {
        std::error_code ec;
        const std::filesystem::path fs_path(json_path);
        if (!fs_path.parent_path().empty()) {
            std::filesystem::create_directories(fs_path.parent_path(), ec);
        }
        std::ofstream os(json_path, std::ios::out | std::ios::trunc);
        if (!os.is_open()) {
            std::fprintf(stderr, "[clock_check] failed to write %s\n", json_path.c_str());
            return 1;
        }
        os << "{\"rounds\":" << rounds << ",\"interval_ms\":" << interval_ms
           << ",\"elapsed_mono_ns\":" << elapsed_mono_ns << ",\"drift_ns\":" << drift_ns
           << ",\"drift_ppm\":" << drift_ppm << ",\"worst_uncertainty_ns\":" << worst_uncertainty
           << ",\"generated_utc\":\"" << argentum::core::to_utc(argentum::core::wall_now_ns())
           << "\"}\n";
        std::printf("json: %s\n", json_path.c_str());
    }

    return 0;
}
