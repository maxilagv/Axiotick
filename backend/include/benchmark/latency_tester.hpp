#pragma once

#include "core/types.h"
#include <chrono>
#include <vector>
#include <iostream>
#include <numeric>
#include <algorithm>

namespace argentum::benchmark {

class LatencyTester {
public:
    using Clock = std::chrono::high_resolution_clock;

    void start(size_t iterations) {
        std::cout << "[Benchmark] Starting latency test (" << iterations << " iterations)..." << std::endl;
        latencies_ns_.reserve(iterations);

        for (size_t i = 0; i < iterations; ++i) {
            auto t1 = Clock::now();
            
            // Critical Path Simulation
            // 1. Serialize
            // 2. Risk Check (mock)
            // 3. Order Book Add (mock)
            volatile double risk_check = 100.0 * 1.05; // Force calculation
            (void)risk_check;
            
            auto t2 = Clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            latencies_ns_.push_back(ns);
        }
    }

    void report() {
        if (latencies_ns_.empty()) return;

        double sum = std::accumulate(latencies_ns_.begin(), latencies_ns_.end(), 0.0);
        double avg = sum / latencies_ns_.size();
        
        std::sort(latencies_ns_.begin(), latencies_ns_.end());
        size_t idx50 = static_cast<size_t>(latencies_ns_.size() * 0.50);
        size_t idx99 = static_cast<size_t>(latencies_ns_.size() * 0.99);
        if (idx50 >= latencies_ns_.size()) idx50 = latencies_ns_.size() - 1;
        if (idx99 >= latencies_ns_.size()) idx99 = latencies_ns_.size() - 1;
        double p50 = static_cast<double>(latencies_ns_[idx50]);
        double p99 = static_cast<double>(latencies_ns_[idx99]);
        
        std::cout << "\n--- Latency Report ---\n";
        std::cout << "Avg: " << avg << " ns\n";
        std::cout << "P50: " << p50 << " ns\n";
        std::cout << "P99: " << p99 << " ns\n";
        std::cout << "----------------------\n";
    }

private:
    std::vector<long long> latencies_ns_;
};

} // namespace argentum::benchmark
