// Data-transport pipeline benchmark: MarketTick construct -> codec encode ->
// bus publish -> SPSC consumer -> codec decode -> DataWriterService enqueue.
// The decision path (signal/EV/risk/OMS) is NOT covered here — that is the
// wire-to-decision report in the demos.
//
// In-band probe convention (benchmark-local): tick.timestamp_ns carries the
// source-time semantic (wall clock) and tick.ingress_ns carries a MONOTONIC
// send stamp, so the consumer measures send->decode transport latency on the
// mono clock without a side-channel array (messages can drop; indices would
// not line up). Production code stamps ingress_ns with wall time — this
// benchmark repurposes the field deliberately and says so.
//
// JSON path: argv[1], default benchmarks_out/pipeline.json.

#include "benchmark/harness.hpp"
#include "bus/message_bus.hpp"
#include "codec/market_tick_codec.hpp"
#include "core/latency_histogram.hpp"
#include "core/time_utils.hpp"
#include "persist/data_writer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    argentum::bus::InprocBusConfig config;
    config.queue_capacity = 131072;
    config.policy = argentum::bus::BackpressurePolicy::DropNewest;
    config.consumer_threads = 1;
    config.busy_poll_during_market_hours = true;
    config.worker_name_prefix = "bench-bus";

    auto bus = argentum::bus::create_inproc_bus(config);

    argentum::persist::DataWriterService writer;
    writer.set_flush_interval_ms(20);
    writer.set_queue_capacity(16384);
    writer.set_overflow_policy(argentum::persist::DataWriterService::OverflowPolicy::DropNewest);
    writer.set_csv_path("data/market_ticks_bench.csv");
    writer.start();

    constexpr uint64_t kWarmup = 50'000;
    constexpr uint64_t kTotal = 1'000'000;

    std::atomic<size_t> consumed{0};
    std::atomic<size_t> published{0};
    std::atomic<size_t> dropped{0};
    // Consumer thread is the only writer during the run; read after quiesce.
    argentum::core::LatencyHistogram transport_hist;
    std::atomic<bool> measuring{false};

    bus->subscribe("market.ticks", [&](const void* data, size_t size) {
        MarketTick tick{};
        if (argentum::codec::decode_market_tick(data, size, &tick) == ARGENTUM_OK) {
            writer.enqueue(tick);
            consumed.fetch_add(1, std::memory_order_relaxed);
            if (measuring.load(std::memory_order_relaxed) && tick.ingress_ns != 0) {
                const uint64_t now = argentum::core::mono_now_ns();
                if (now >= tick.ingress_ns) {
                    transport_hist.record(now - tick.ingress_ns);
                }
            }
        }
    });

    std::vector<uint8_t> payload;
    payload.reserve(argentum::bus::kMaxMessageSize);

    auto publish_one = [&](uint64_t i) {
        MarketTick tick{};
        tick.timestamp_ns = argentum::core::wall_now_ns();
        tick.ingress_ns = argentum::core::mono_now_ns();  // in-band mono probe (see header comment)
        tick.price = 100.0 + static_cast<double>(i % 1000) * 0.01;
        tick.quantity = 1.0;
        std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "SIM", sizeof(tick.source) - 1);
        tick.side = SIDE_BUY;

#ifdef ARGENTUM_USE_FLATBUFFERS
        if (argentum::codec::encode_market_tick_flatbuffers(tick, &payload, false) == ARGENTUM_OK) {
#else
        if (argentum::codec::encode_market_tick_legacy(tick, &payload) == ARGENTUM_OK) {
#endif
            if (bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK) {
                published.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    // Warmup: exercises codec, ring, consumer and writer before measurement.
    for (uint64_t i = 0; i < kWarmup; ++i) {
        publish_one(i);
    }
    while (consumed.load(std::memory_order_relaxed) < published.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    measuring.store(true, std::memory_order_relaxed);
    const size_t published_before = published.load(std::memory_order_relaxed);
    const uint64_t start_ns = argentum::core::mono_now_ns();
    for (uint64_t i = 0; i < kTotal; ++i) {
        publish_one(kWarmup + i);
    }
    const uint64_t publish_end_ns = argentum::core::mono_now_ns();

    while (consumed.load(std::memory_order_relaxed) < published.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    measuring.store(false, std::memory_order_relaxed);
    writer.stop();
    const uint64_t end_ns = argentum::core::mono_now_ns();

    const size_t published_measured = published.load(std::memory_order_relaxed) - published_before;
    const double publish_ms = static_cast<double>(publish_end_ns - start_ns) / 1e6;
    const double end_to_end_ms = static_cast<double>(end_ns - start_ns) / 1e6;
    const double throughput =
        (publish_ms > 0.0) ? (static_cast<double>(published_measured) / (publish_ms / 1000.0)) : 0.0;

    argentum::bus::TopicMetrics metrics{};
    bus->get_metrics("market.ticks", &metrics);

    argentum::benchmark::BenchmarkReporter reporter("pipeline_benchmark");
    argentum::benchmark::BenchmarkCase result;
    result.name = "tick_encode_publish_consume_decode";
    result.warmup_iterations = kWarmup;
    result.iterations = kTotal;
    result.latency = transport_hist.report();
    result.throughput_per_sec = throughput;
    result.extra.emplace_back("published", std::to_string(published_measured));
    result.extra.emplace_back("dropped_publish", std::to_string(dropped.load()));
    result.extra.emplace_back("bus_drops", std::to_string(metrics.drops));
    result.extra.emplace_back("bus_backpressure_hits", std::to_string(metrics.backpressure_hits));
    result.extra.emplace_back("bus_queue_depth_end", std::to_string(metrics.queue_depth));
    result.extra.emplace_back("bus_publish_p99_ns", std::to_string(metrics.publish_latency_ns_p99));
    result.extra.emplace_back("publish_time_ms", std::to_string(publish_ms));
    result.extra.emplace_back("end_to_end_ms", std::to_string(end_to_end_ms));
#ifdef ARGENTUM_USE_FLATBUFFERS
    result.extra.emplace_back("codec", "flatbuffers_v2");
#else
    result.extra.emplace_back("codec", "legacy_v1");
#endif
#ifdef ARGENTUM_ENABLE_LATENCY_TRACE
    result.extra.emplace_back("latency_trace", "on");
#else
    result.extra.emplace_back("latency_trace", "off");
#endif
    reporter.add(result);

    reporter.print_human(stdout);
    const char* json_path = (argc > 1) ? argv[1] : "benchmarks_out/pipeline.json";
    if (!reporter.write_json(json_path)) {
        std::fprintf(stderr, "[pipeline_benchmark] failed to write %s\n", json_path);
        return 1;
    }
    std::printf("[pipeline_benchmark] json: %s\n", json_path);
    return 0;
}
