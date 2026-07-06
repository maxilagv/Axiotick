#include "persist/data_writer.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>

int main() {
    const std::filesystem::path out = "data/test_market_ticks.csv";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    argentum::persist::DataWriterService writer;
    writer.set_csv_path(out.string());
    writer.set_max_batch(2);
    writer.set_flush_interval_ms(5);
    writer.set_queue_capacity(16);
    writer.start();

    for (int i = 0; i < 5; ++i) {
        MarketTick tick{};
        tick.timestamp_ns = 1'700'000'000'000'000'000ULL + static_cast<uint64_t>(i);
        tick.price = 100.0 + static_cast<double>(i);
        tick.quantity = 1.0;
        tick.side = SIDE_BUY;
        std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "TEST", sizeof(tick.source) - 1);
        writer.enqueue(tick);
    }

    writer.stop();

    assert(std::filesystem::exists(out));
    auto size = std::filesystem::file_size(out, ec);
    assert(!ec);
    assert(size > 0);
    return 0;
}
