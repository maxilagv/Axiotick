#pragma once

#include "gateway/venue.hpp"

#include <string>
#include <unordered_map>

namespace argentum::gateway {

class FixAdapterV1 {
public:
    explicit FixAdapterV1(std::string venue_id);

    const std::string& venue_id() const;

    bool parse_market_data(const std::string& raw, VenueQuote* out_quote) const;

    static bool parse_fields(
        const std::string& raw,
        std::unordered_map<int, std::string>* out_fields,
        char delimiter = '\x01');

    static std::string normalize_symbol(const std::string& raw_symbol);

private:
    std::string venue_id_;
};

} // namespace argentum::gateway
