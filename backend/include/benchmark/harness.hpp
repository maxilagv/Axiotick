#pragma once

// Benchmark harness v2 (ADR 0015).
//
// Every Axiotick benchmark runs through this harness so that results are
// comparable by construction:
//   - explicit warmup phase, excluded from statistics;
//   - all timing through core::mono_now_ns() (one clock base; the banned
//     alias clock the benchmarks used to call is gone);
//   - percentiles from core::LatencyHistogram (same math as runtime);
//   - machine-readable JSON output whose environment metadata is captured
//     programmatically — CPU, cores, RAM, OS, compiler, build config — so the
//     tables in docs/benchmarks/ are pasted from tool output, never typed.
//
// scripts/check_benchmarks.py compares the JSON against
// docs/benchmarks/thresholds.json and fails on regression.

#include "core/latency_histogram.hpp"
#include "core/time_utils.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

namespace argentum::benchmark {

struct EnvironmentMetadata {
    std::string cpu_model;
    unsigned hardware_threads = 0;
    uint64_t total_ram_mb = 0;
    std::string os;
    std::string compiler;
    std::string build_config;
    std::string timestamp_utc;
    uint64_t clock_uncertainty_ns = 0;

    static EnvironmentMetadata capture() {
        EnvironmentMetadata env{};
        env.cpu_model = cpu_brand_string();
        env.hardware_threads = std::thread::hardware_concurrency();
        env.total_ram_mb = total_ram_megabytes();
        env.os = os_description();
        env.compiler = compiler_description();
#if defined(ARGENTUM_BUILD_CONFIG)
        env.build_config = ARGENTUM_BUILD_CONFIG;
#else
        env.build_config = "unknown";
#endif
        env.timestamp_utc = core::to_utc(core::wall_now_ns());
        env.clock_uncertainty_ns = core::ClockCalibration::measure().uncertainty_ns;
        return env;
    }

private:
    static std::string cpu_brand_string() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        int regs[4] = {0, 0, 0, 0};
        char brand[49] = {0};
#if defined(_WIN32)
        __cpuid(regs, 0x80000000);
        if (static_cast<unsigned>(regs[0]) >= 0x80000004) {
            for (int i = 0; i < 3; ++i) {
                __cpuid(regs, 0x80000002 + i);
                std::memcpy(brand + i * 16, regs, sizeof(regs));
            }
            return trim(brand);
        }
#else
        unsigned int a = 0, b = 0, c = 0, d = 0;
        if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
            unsigned int* out = reinterpret_cast<unsigned int*>(brand);
            for (unsigned int leaf = 0; leaf < 3; ++leaf) {
                __get_cpuid(0x80000002 + leaf, &a, &b, &c, &d);
                out[leaf * 4 + 0] = a;
                out[leaf * 4 + 1] = b;
                out[leaf * 4 + 2] = c;
                out[leaf * 4 + 3] = d;
            }
            return trim(brand);
        }
#endif
#endif
        return "unknown-cpu";
    }

    static uint64_t total_ram_megabytes() {
#if defined(_WIN32)
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            return static_cast<uint64_t>(status.ullTotalPhys / (1024ULL * 1024ULL));
        }
        return 0;
#else
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages <= 0 || page_size <= 0) return 0;
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size) / (1024ULL * 1024ULL);
#endif
    }

    static std::string os_description() {
#if defined(_WIN32)
        // RtlGetVersion reports the true build regardless of manifest.
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
                if (fn(&info) == 0) {
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "Windows %lu.%lu build %lu",
                                  static_cast<unsigned long>(info.dwMajorVersion),
                                  static_cast<unsigned long>(info.dwMinorVersion),
                                  static_cast<unsigned long>(info.dwBuildNumber));
                    return buffer;
                }
            }
        }
        return "Windows";
#else
        utsname raw{};
        if (uname(&raw) == 0) {
            return std::string(raw.sysname) + " " + raw.release;
        }
        return "POSIX";
#endif
    }

    static std::string compiler_description() {
        char buffer[64];
#if defined(_MSC_VER)
        std::snprintf(buffer, sizeof(buffer), "MSVC %d", _MSC_FULL_VER);
#elif defined(__clang__)
        std::snprintf(buffer, sizeof(buffer), "clang %d.%d.%d", __clang_major__, __clang_minor__,
                      __clang_patchlevel__);
#elif defined(__GNUC__)
        std::snprintf(buffer, sizeof(buffer), "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__,
                      __GNUC_PATCHLEVEL__);
#else
        std::snprintf(buffer, sizeof(buffer), "unknown-compiler");
#endif
        return buffer;
    }

    static std::string trim(const char* raw) {
        std::string out(raw);
        const size_t begin = out.find_first_not_of(' ');
        if (begin == std::string::npos) return out;
        const size_t end = out.find_last_not_of(' ');
        return out.substr(begin, end - begin + 1);
    }
};

struct BenchmarkCase {
    std::string name;
    uint64_t warmup_iterations = 0;
    uint64_t iterations = 0;
    core::LatencyReport latency;
    double throughput_per_sec = 0.0;
    std::vector<std::pair<std::string, std::string>> extra;
};

/// Runs `fn` warmup+measured times, timing each measured call with
/// core::mono_now_ns() into a LatencyHistogram.
template <typename Fn>
core::LatencyReport measure_loop(uint64_t warmup, uint64_t iterations, Fn&& fn,
                                 core::LatencyHistogram* out_hist = nullptr) {
    core::LatencyHistogram local;
    core::LatencyHistogram& hist = out_hist ? *out_hist : local;

    for (uint64_t i = 0; i < warmup; ++i) {
        fn(i);
    }
    for (uint64_t i = 0; i < iterations; ++i) {
        const uint64_t start = core::mono_now_ns();
        fn(warmup + i);
        hist.record(core::mono_now_ns() - start);
    }
    return hist.report();
}

class BenchmarkReporter {
public:
    explicit BenchmarkReporter(std::string suite_name)
        : suite_name_(std::move(suite_name)), env_(EnvironmentMetadata::capture()) {}

    void add(BenchmarkCase result) { cases_.push_back(std::move(result)); }

    const EnvironmentMetadata& environment() const { return env_; }

    void print_human(std::FILE* out) const {
        std::fprintf(out, "[%s] env: %s | threads=%u | ram=%" PRIu64 "MB | %s | %s | %s\n",
                     suite_name_.c_str(), env_.cpu_model.c_str(), env_.hardware_threads,
                     env_.total_ram_mb, env_.os.c_str(), env_.compiler.c_str(),
                     env_.build_config.c_str());
        std::fprintf(out, "[%s] clock_uncertainty=%" PRIu64 "ns | %s\n", suite_name_.c_str(),
                     env_.clock_uncertainty_ns, env_.timestamp_utc.c_str());
        for (const BenchmarkCase& c : cases_) {
            std::fprintf(out,
                         "[%s] %s: iters=%" PRIu64 " (warmup=%" PRIu64 ") p50=%" PRIu64
                         "ns p95=%" PRIu64 "ns p99=%" PRIu64 "ns p99.9=%" PRIu64 "ns max=%" PRIu64
                         "ns\n",
                         suite_name_.c_str(), c.name.c_str(), c.iterations, c.warmup_iterations,
                         c.latency.p50_ns, c.latency.p95_ns, c.latency.p99_ns, c.latency.p999_ns,
                         c.latency.max_ns);
            if (c.throughput_per_sec > 0.0) {
                std::fprintf(out, "[%s] %s: throughput=%.0f/sec\n", suite_name_.c_str(),
                             c.name.c_str(), c.throughput_per_sec);
            }
            for (const auto& [key, value] : c.extra) {
                std::fprintf(out, "[%s] %s: %s=%s\n", suite_name_.c_str(), c.name.c_str(),
                             key.c_str(), value.c_str());
            }
        }
    }

    bool write_json(const std::string& path) const {
        std::error_code ec;
        const std::filesystem::path fs_path(path);
        if (!fs_path.parent_path().empty()) {
            std::filesystem::create_directories(fs_path.parent_path(), ec);
        }
        std::ofstream os(path, std::ios::out | std::ios::trunc);
        if (!os.is_open()) return false;

        os << "{\n  \"suite\":\"" << json_escape(suite_name_) << "\",\n  \"environment\":{"
           << "\"cpu\":\"" << json_escape(env_.cpu_model) << "\""
           << ",\"hardware_threads\":" << env_.hardware_threads
           << ",\"total_ram_mb\":" << env_.total_ram_mb << ",\"os\":\"" << json_escape(env_.os)
           << "\"" << ",\"compiler\":\"" << json_escape(env_.compiler) << "\""
           << ",\"build_config\":\"" << json_escape(env_.build_config) << "\""
           << ",\"timestamp_utc\":\"" << json_escape(env_.timestamp_utc) << "\""
           << ",\"clock_uncertainty_ns\":" << env_.clock_uncertainty_ns << "},\n  \"cases\":[";

        bool first = true;
        for (const BenchmarkCase& c : cases_) {
            if (!first) os << ",";
            first = false;
            os << "\n    {\"name\":\"" << json_escape(c.name) << "\""
               << ",\"warmup_iterations\":" << c.warmup_iterations
               << ",\"iterations\":" << c.iterations << ",\"samples\":" << c.latency.samples
               << ",\"min_ns\":" << c.latency.min_ns << ",\"p50_ns\":" << c.latency.p50_ns
               << ",\"p95_ns\":" << c.latency.p95_ns << ",\"p99_ns\":" << c.latency.p99_ns
               << ",\"p999_ns\":" << c.latency.p999_ns << ",\"max_ns\":" << c.latency.max_ns
               << ",\"mean_ns\":" << c.latency.mean_ns
               << ",\"throughput_per_sec\":" << c.throughput_per_sec;
            for (const auto& [key, value] : c.extra) {
                os << ",\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
            }
            os << "}";
        }
        os << "\n  ]\n}\n";
        return os.good();
    }

private:
    static std::string json_escape(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (const char c : raw) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
            }
        }
        return out;
    }

    std::string suite_name_;
    EnvironmentMetadata env_;
    std::vector<BenchmarkCase> cases_;
};

} // namespace argentum::benchmark
