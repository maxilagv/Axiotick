#include "backtest/backtest_engine.hpp"

#include "audit/logger.hpp"
#include "core/fixed_point.hpp"
#include "engine/order_book.hpp"
#include "persist/event_journal.hpp"
#include "regime/bar_aggregator.hpp"
#include "trading/order_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace argentum::backtest {

namespace {
bool parse_i64_json_field(const std::string& json, const std::string& key, int64_t* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\":";
    const size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    const size_t begin = pos + marker.size();
    size_t end = begin;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == begin) return false;

    *out = static_cast<int64_t>(std::strtoll(json.substr(begin, end - begin).c_str(), nullptr, 10));
    return true;
}

std::string to_compact_symbol(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '/' || c == '-' || c == '_' || c == ' ' || c == '.') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

// Shared JSONL fill parser: used by load_trades_from_journal (Mode A input),
// by run_strategy to reconstruct its own fills, and by the walk-forward
// aggregator to concatenate per-fold journals.
bool BacktestEngine::parse_trades_from_journal(
    const std::string& journal_path,
    std::vector<HistoricalTrade>* out) {
    if (!out) return false;
    std::ifstream in(journal_path);
    if (!in.is_open()) return false;

    std::string line;
    bool loaded = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.find("\"type\":\"trade_executed\"") == std::string::npos) continue;

        int64_t price_ticks = 0;
        int64_t quantity_lots = 0;
        int64_t side = 0;
        int64_t related_signal_id = 0;
        if (!parse_i64_json_field(line, "price_ticks", &price_ticks)) continue;
        if (!parse_i64_json_field(line, "quantity_lots", &quantity_lots)) continue;
        if (!parse_i64_json_field(line, "side", &side)) continue;
        (void)parse_i64_json_field(line, "related_signal_id", &related_signal_id);
        if (price_ticks <= 0 || quantity_lots <= 0) continue;

        out->push_back(HistoricalTrade{
            price_ticks,
            quantity_lots,
            static_cast<uint8_t>(side),
            static_cast<uint64_t>(related_signal_id < 0 ? 0 : related_signal_id)
        });
        loaded = true;
    }

    return loaded;
}

void BacktestEngine::load_data(const std::string& symbol, const std::string& start_date, const std::string& end_date) {
    (void)start_date;
    (void)end_date;

    history_.clear();
    trades_.clear();

    ARGENTUM_LOG(INFO, "[Backtest] Loading persisted dataset for " << symbol << "...");
    const bool ticks_ok = load_ticks_from_csv("data/market_ticks.csv", symbol);
    const bool trades_ok = load_trades_from_journal("data/order_events.jsonl");

    if (!ticks_ok) {
        ARGENTUM_LOG(WARN, "[Backtest] no tick CSV found; continuing with execution events only.");
    }
    if (!trades_ok) {
        ARGENTUM_LOG(WARN, "[Backtest] no event journal found; running with tick-only replay.");
    }

    ARGENTUM_LOG(INFO, "[Backtest] Loaded ticks=" << history_.size() << " trades=" << trades_.size());
}

bool BacktestEngine::load_ticks_from_csv(const std::string& csv_path, const std::string& symbol) {
    std::ifstream in(csv_path);
    if (!in.is_open()) return false;

    const std::string symbol_filter = to_compact_symbol(symbol);
    std::string line;
    bool loaded = false;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("timestamp_ns,", 0) == 0) continue;

        std::stringstream ss(line);
        std::string ts_s;
        std::string symbol_s;
        std::string price_s;
        std::string qty_s;
        std::string side_s;
        std::string source_s;

        if (!std::getline(ss, ts_s, ',')) continue;
        if (!std::getline(ss, symbol_s, ',')) continue;
        if (!std::getline(ss, price_s, ',')) continue;
        if (!std::getline(ss, qty_s, ',')) continue;
        if (!std::getline(ss, side_s, ',')) continue;
        if (!std::getline(ss, source_s, ',')) source_s.clear();

        if (!symbol_filter.empty() && to_compact_symbol(symbol_s) != symbol_filter) {
            continue;
        }

        MarketTick tick{};
        tick.timestamp_ns = static_cast<uint64_t>(std::strtoull(ts_s.c_str(), nullptr, 10));
        tick.price = std::strtod(price_s.c_str(), nullptr);
        tick.quantity = std::strtod(qty_s.c_str(), nullptr);
        tick.side = static_cast<uint8_t>(
            (!side_s.empty() && (side_s[0] == 'B' || side_s[0] == 'b')) ? SIDE_BUY : SIDE_SELL);
        std::strncpy(tick.symbol, symbol_s.c_str(), sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, source_s.c_str(), sizeof(tick.source) - 1);

        if (tick.timestamp_ns == 0 || tick.price <= 0.0 || tick.quantity <= 0.0) {
            continue;
        }

        history_.push_back(tick);
        loaded = true;
    }

    std::sort(history_.begin(), history_.end(), [](const MarketTick& a, const MarketTick& b) {
        return a.timestamp_ns < b.timestamp_ns;
    });

    return loaded;
}

bool BacktestEngine::load_trades_from_journal(const std::string& journal_path) {
    return parse_trades_from_journal(journal_path, &trades_);
}

void BacktestEngine::load_ticks(std::vector<MarketTick> ticks) {
    history_ = std::move(ticks);
    std::sort(history_.begin(), history_.end(), [](const MarketTick& a, const MarketTick& b) {
        return a.timestamp_ns < b.timestamp_ns;
    });
}

BacktestResult BacktestEngine::run(std::shared_ptr<analysis::Strategy> strategy) {
    if (!strategy) {
        return BacktestResult{0.0, 0, 0.0, 0.0};
    }

    ARGENTUM_LOG(INFO, "[Backtest] Running persisted replay for strategy: " << strategy->get_name());

    for (const auto& tick : history_) {
        strategy->on_tick(tick);
    }

    if (history_.empty() && trades_.empty()) {
        return BacktestResult{0.0, 0, 0.0, 0.0};
    }

    const EquityCurveMetrics metrics = compute_equity_curve_metrics(trades_, initial_capital_);
    return BacktestResult{
        metrics.total_pnl,
        trades_.size(),
        metrics.max_drawdown,
        metrics.sharpe_ratio
    };
}

MonteCarloReport BacktestEngine::run_monte_carlo(size_t resamples, uint64_t seed) const {
    return run_monte_carlo_bootstrap(trades_, initial_capital_, resamples, seed);
}

StrategyBacktestReport BacktestEngine::run_strategy(
    std::shared_ptr<signal::CandidateStrategy> strategy,
    const StrategyBacktestConfig& config) {
    StrategyBacktestReport report{};
    if (!strategy || history_.empty()) {
        return report;
    }

    const std::string symbol = config.symbol.empty()
        ? std::string(history_.front().symbol)
        : config.symbol;

    // The journal is a per-run scratch artifact: recreate it so reconstructed
    // fills belong to this run only.
    std::remove(config.journal_path.c_str());

    auto book = std::make_shared<engine::OrderBook>(symbol);
    auto risk = std::make_shared<risk::RiskManager>(config.risk_limits);
    auto journal = std::make_shared<persist::EventJournal>(config.journal_path);
    auto oms = std::make_shared<trading::OrderManager>(risk, book, journal);

    signal::SignalEngine::Config engine_config{};
    engine_config.gate = config.gate;
    engine_config.registry = config.registry;
    engine_config.first_signal_id = config.first_signal_id;
    engine_config.first_order_id = config.first_order_id;
    signal::SignalEngine engine(engine_config, oms);
    engine.registry().register_strategy(strategy->strategy_id(), config.registration);

    regime::BarAggregator bars(config.bar_duration_ns);
    regime::RegimeClassifier classifier(config.regime);
    regime::RegimeLabel last_notified_label = regime::RegimeLabel::Unknown;

    uint64_t maker_id = config.maker_base_order_id;

    ARGENTUM_LOG(
        INFO,
        "[Backtest] Counterfactual run strategy=" << strategy->strategy_id()
        << " symbol=" << symbol
        << " ticks=" << history_.size());

    for (const MarketTick& tick : history_) {
        ++report.ticks_processed;
        risk->maybe_roll_day(tick.timestamp_ns);
        risk->mark_to_market(symbol, tick.price);
        engine.on_market_tick(symbol, tick.price, tick.timestamp_ns);

        if (auto closed = bars.on_tick(tick)) {
            classifier.on_bar(*closed);
            ++report.bars_closed;
            const auto label = classifier.current_label();
            const size_t label_index = static_cast<size_t>(label);
            if (label_index < report.regime_bar_counts.size()) {
                ++report.regime_bar_counts[label_index];
            }
            if (label != last_notified_label) {
                strategy->on_regime_change(label, classifier.current_confidence());
                last_notified_label = label;
            }
        }

        auto candidate = strategy->on_tick(tick);
        if (!candidate) {
            continue;
        }

        candidate->regime = classifier.current_label();
        candidate->regime_confidence = classifier.current_confidence();

        // Synthetic counterparty liquidity (not our risk): seeded directly
        // into the book AT the reference tick price, then any unexecuted
        // remainder is removed so it cannot interact with later candidates
        // at a stale price. v1 limitation (no spread/depth model) documented
        // in StrategyBacktestConfig.
        Order maker{};
        maker.order_id = ++maker_id;
        maker.timestamp_ns = tick.timestamp_ns;
        maker.price = candidate->price;
        maker.quantity = std::min(
            candidate->quantity * config.synthetic_liquidity_multiplier,
            core::kMaxSafeQuantity);
        core::normalize_order_scalars(&maker);
        std::strncpy(maker.symbol, candidate->symbol.c_str(), sizeof(maker.symbol) - 1);
        maker.side = static_cast<uint8_t>((candidate->side == SIDE_BUY) ? SIDE_SELL : SIDE_BUY);
        maker.type = ORDER_TYPE_LIMIT;
        maker.tif = TIF_GTC;
        (void)book->add_order(maker);

        (void)engine.process(*candidate);

        (void)book->cancel_order(maker.order_id);
    }

    report.funnel = engine.stats();

    // Resolve every evaluation whose horizon falls beyond the last tick —
    // dropping them silently would understate the sample.
    engine.registry().flush_pending(symbol, history_.back().price, history_.back().timestamp_ns);

    const std::vector<strategy::ResolvedEvaluation> resolved =
        engine.registry().resolved_history(strategy->strategy_id());
    report.resolved_evaluations = resolved.size();
    for (const strategy::ResolvedEvaluation& evaluation : resolved) {
        const size_t index = static_cast<size_t>(evaluation.regime);
        if (index >= report.by_regime.size()) continue;
        RegimeMetrics& bucket = report.by_regime[index];
        ++bucket.evaluation_count;
        bucket.mean_realized_bps += evaluation.realized_bps;
        bucket.mean_predicted_ev_bps += evaluation.predicted_ev_net_bps;
        if (evaluation.realized_bps > 0.0) {
            bucket.hit_rate += 1.0;
        }
    }
    for (RegimeMetrics& bucket : report.by_regime) {
        if (bucket.evaluation_count == 0) continue;
        const double n = static_cast<double>(bucket.evaluation_count);
        bucket.mean_realized_bps /= n;
        bucket.mean_predicted_ev_bps /= n;
        bucket.hit_rate /= n;
    }

    report.lifecycle_transitions = engine.registry().transition_history(strategy->strategy_id());
    report.final_health = engine.registry().health(strategy->strategy_id());

    journal->flush();
    std::vector<HistoricalTrade> fills;
    (void)parse_trades_from_journal(config.journal_path, &fills);
    report.trades_loaded = fills.size();
    report.metrics = compute_equity_curve_metrics(fills, config.initial_capital);

    ARGENTUM_LOG(
        INFO,
        "[Backtest] Counterfactual done candidates=" << report.funnel.candidates
        << " accepted=" << report.funnel.gate_accepted
        << " fills=" << report.trades_loaded
        << " pnl=" << report.metrics.total_pnl);

    return report;
}

} // namespace argentum::backtest
