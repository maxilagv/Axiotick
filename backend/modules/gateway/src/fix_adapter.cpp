#include "gateway/fix_adapter.hpp"

#include "core/fixed_point.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace argentum::gateway {

FixAdapterV1::FixAdapterV1(std::string venue_id) : venue_id_(std::move(venue_id)) {}

const std::string& FixAdapterV1::venue_id() const {
    return venue_id_;
}

bool FixAdapterV1::parse_market_data(const std::string& raw, VenueQuote* out_quote) const {
    if (!out_quote || raw.empty()) return false;

    std::unordered_map<int, std::string> fields;
    const char delimiter = (raw.find('|') != std::string::npos) ? '|' : '\x01';
    if (!parse_fields(raw, &fields, delimiter)) return false;

    auto symbol_it = fields.find(55);
    if (symbol_it == fields.end()) return false;

    const auto bid_px_it = fields.find(132);
    const auto ask_px_it = fields.find(133);
    const auto bid_qty_it = fields.find(134);
    const auto ask_qty_it = fields.find(135);

    if (bid_px_it == fields.end() || ask_px_it == fields.end() ||
        bid_qty_it == fields.end() || ask_qty_it == fields.end()) {
        return false;
    }

    const double bid_px = std::strtod(bid_px_it->second.c_str(), nullptr);
    const double ask_px = std::strtod(ask_px_it->second.c_str(), nullptr);
    const double bid_qty = std::strtod(bid_qty_it->second.c_str(), nullptr);
    const double ask_qty = std::strtod(ask_qty_it->second.c_str(), nullptr);
    if (bid_px <= 0.0 || ask_px <= 0.0 || bid_qty <= 0.0 || ask_qty <= 0.0) return false;

    VenueQuote quote{};
    quote.venue_id = venue_id_;
    quote.symbol = normalize_symbol(symbol_it->second);
    quote.bid_price_ticks = core::to_price_ticks(bid_px);
    quote.ask_price_ticks = core::to_price_ticks(ask_px);
    quote.bid_size_lots = core::to_quantity_lots(bid_qty);
    quote.ask_size_lots = core::to_quantity_lots(ask_qty);

    auto ts_it = fields.find(60);
    if (ts_it != fields.end()) {
        quote.timestamp_ns = static_cast<uint64_t>(std::strtoull(ts_it->second.c_str(), nullptr, 10));
    }

    if (quote.symbol.empty() || quote.bid_price_ticks <= 0 || quote.ask_price_ticks <= 0 ||
        quote.bid_size_lots <= 0 || quote.ask_size_lots <= 0) {
        return false;
    }

    *out_quote = std::move(quote);
    return true;
}

bool FixAdapterV1::parse_fields(
    const std::string& raw,
    std::unordered_map<int, std::string>* out_fields,
    char delimiter) {
    if (!out_fields || raw.empty()) return false;

    out_fields->clear();
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find(delimiter, start);
        if (end == std::string::npos) end = raw.size();

        if (end > start) {
            const std::string token = raw.substr(start, end - start);
            const size_t eq = token.find('=');
            if (eq != std::string::npos && eq > 0) {
                const int tag = std::atoi(token.substr(0, eq).c_str());
                if (tag > 0) {
                    (*out_fields)[tag] = token.substr(eq + 1);
                }
            }
        }

        if (end == raw.size()) break;
        start = end + 1;
    }

    return !out_fields->empty();
}

std::string FixAdapterV1::normalize_symbol(const std::string& raw_symbol) {
    std::string compact;
    compact.reserve(raw_symbol.size());
    for (char c : raw_symbol) {
        if (c == '/' || c == '-' || c == '_' || c == ' ') continue;
        compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (compact.size() == 6) {
        return compact.substr(0, 3) + "/" + compact.substr(3, 3);
    }
    if (compact.size() >= 7 && compact.rfind("USDT") == compact.size() - 4) {
        return compact.substr(0, compact.size() - 4) + "/USDT";
    }
    return compact;
}

} // namespace argentum::gateway
