#include "audit/logger.hpp"
#include "persist/event_journal.hpp"

#include <string>

int main(int argc, char** argv) {
    std::string path = "data/order_events.jsonl";
    if (argc > 1 && argv[1] != nullptr) {
        path = argv[1];
    }

    argentum::persist::ReplaySummary summary{};
    if (!argentum::persist::EventReplayer::replay_file(path, &summary)) {
        ARGENTUM_LOG(ERROR, "[Replay] Failed to replay file: " << path);
        return 1;
    }

    ARGENTUM_LOG(INFO, "[Replay] file=" << path);
    ARGENTUM_LOG(INFO, "[Replay] total_events=" << summary.total_events);
    ARGENTUM_LOG(
        INFO,
        "[Replay] accepted=" << summary.accepted
        << " rejected=" << summary.rejected
        << " gateway_rejected=" << summary.gateway_rejected
        << " trades=" << summary.trades
        << " canceled=" << summary.canceled
        << " replaced=" << summary.replaced);
    ARGENTUM_LOG(
        INFO,
        "[Replay] active_orders=" << summary.active_orders.size()
        << " order_history=" << summary.order_history.size());
    ARGENTUM_LOG(
        INFO,
        "[Replay] monotonic_seq=" << (summary.monotonic_seq ? "true" : "false")
        << " monotonic_time=" << (summary.monotonic_time ? "true" : "false")
        << " sequence_gaps=" << summary.sequence_gaps);
    ARGENTUM_LOG(
        INFO,
        "[Replay] committed_exposure_units=" << summary.committed_exposure_units
        << " filled_exposure_units=" << summary.filled_exposure_units
        << " net_position_lots=" << summary.net_position_lots);
    return 0;
}
