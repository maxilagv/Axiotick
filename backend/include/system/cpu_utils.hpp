#pragma once

#include "audit/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <filesystem>
#include <pthread.h>
#include <sched.h>
#endif

namespace argentum::system {

namespace detail {

#ifdef _WIN32
inline bool set_affinity(HANDLE thread, int core_id) {
    if (core_id < 0 || core_id >= static_cast<int>(sizeof(DWORD_PTR) * 8)) {
        ARGENTUM_LOG(WARN, "[System] Invalid core id for affinity " << core_id);
        return false;
    }

    const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << core_id);
    if (SetThreadAffinityMask(thread, mask) == 0) {
        ARGENTUM_LOG(WARN, "[System] Failed to pin thread to core " << core_id);
        return false;
    }
    return true;
}

inline int map_windows_priority(int priority) {
    if (priority >= 90) {
        return THREAD_PRIORITY_TIME_CRITICAL;
    }
    if (priority >= 60) {
        return THREAD_PRIORITY_HIGHEST;
    }
    if (priority >= 30) {
        return THREAD_PRIORITY_ABOVE_NORMAL;
    }
    if (priority <= 0) {
        return THREAD_PRIORITY_NORMAL;
    }
    return THREAD_PRIORITY_NORMAL;
}

inline bool set_priority(HANDLE thread, int priority) {
    if (!SetThreadPriority(thread, map_windows_priority(priority))) {
        ARGENTUM_LOG(WARN, "[System] Failed to set thread priority " << priority);
        return false;
    }
    return true;
}

inline bool set_name(HANDLE thread, const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }

    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
    if (!fn) {
        return false;
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (required <= 0) {
        return false;
    }

    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), required) <= 0) {
        return false;
    }

    return SUCCEEDED(fn(thread, wide.c_str()));
}
#else
inline bool set_affinity(pthread_t thread, int core_id) {
    if (core_id < 0) {
        ARGENTUM_LOG(WARN, "[System] Invalid core id for affinity " << core_id);
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        ARGENTUM_LOG(WARN, "[System] Failed to pin thread to core " << core_id);
        return false;
    }
    return true;
}

inline bool set_priority(pthread_t thread, int priority) {
    if (priority <= 0) {
        return true;
    }

    sched_param param{};
    param.sched_priority = std::min(std::max(priority, 1), 99);
    if (pthread_setschedparam(thread, SCHED_FIFO, &param) != 0) {
        ARGENTUM_LOG(WARN, "[System] Failed to set realtime priority " << param.sched_priority);
        return false;
    }
    return true;
}

inline bool set_name(pthread_t thread, const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }

    std::string trimmed{name};
    if (trimmed.size() > 15) {
        trimmed.resize(15);
    }
    return pthread_setname_np(thread, trimmed.c_str()) == 0;
}
#endif

} // namespace detail

inline bool set_thread_affinity(int core_id) {
#ifdef _WIN32
    return detail::set_affinity(GetCurrentThread(), core_id);
#else
    return detail::set_affinity(pthread_self(), core_id);
#endif
}

inline bool set_thread_affinity(std::thread& thread, int core_id) {
    if (!thread.joinable()) {
        return false;
    }
#ifdef _WIN32
    return detail::set_affinity(thread.native_handle(), core_id);
#else
    return detail::set_affinity(thread.native_handle(), core_id);
#endif
}

inline bool set_thread_realtime_priority(int priority = 99) {
#ifdef _WIN32
    return detail::set_priority(GetCurrentThread(), priority);
#else
    return detail::set_priority(pthread_self(), priority);
#endif
}

inline bool set_thread_realtime_priority(std::thread& thread, int priority = 99) {
    if (!thread.joinable()) {
        return false;
    }
    return detail::set_priority(thread.native_handle(), priority);
}

inline bool set_thread_name(const char* name) {
#ifdef _WIN32
    return detail::set_name(GetCurrentThread(), name);
#else
    return detail::set_name(pthread_self(), name);
#endif
}

inline bool set_thread_name(std::thread& thread, const char* name) {
    if (!thread.joinable()) {
        return false;
    }
    return detail::set_name(thread.native_handle(), name);
}

inline bool disable_cpu_frequency_scaling() {
#ifdef _WIN32
    ARGENTUM_LOG(INFO, "[System] CPU frequency scaling is managed by Windows power policy.");
    return true;
#else
    namespace fs = std::filesystem;

    const fs::path cpu_root{"/sys/devices/system/cpu"};
    if (!fs::exists(cpu_root)) {
        ARGENTUM_LOG(WARN, "[System] CPU governor path not available.");
        return false;
    }

    bool changed = false;
    for (const auto& entry : fs::directory_iterator(cpu_root)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string cpu_name = entry.path().filename().string();
        if (cpu_name.size() < 4 || cpu_name.rfind("cpu", 0) != 0) {
            continue;
        }
        bool numeric_suffix = true;
        for (size_t i = 3; i < cpu_name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(cpu_name[i]))) {
                numeric_suffix = false;
                break;
            }
        }
        if (!numeric_suffix) {
            continue;
        }

        const fs::path governor = entry.path() / "cpufreq" / "scaling_governor";
        if (!fs::exists(governor)) {
            continue;
        }

        std::ofstream out(governor);
        if (!out.good()) {
            ARGENTUM_LOG(WARN, "[System] Cannot set governor for " << governor.string());
            continue;
        }
        out << "performance";
        changed = true;
    }

    if (!changed) {
        ARGENTUM_LOG(WARN, "[System] No CPU governors updated.");
    }
    return changed;
#endif
}

inline void pin_thread_to_core(int core_id) {
    (void)set_thread_affinity(core_id);
}

} // namespace argentum::system
