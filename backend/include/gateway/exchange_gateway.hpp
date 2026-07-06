#pragma once

#include "audit/logger.hpp"
#include <string>
#include "core/types.h"

namespace argentum::gateway {

/**
 * @brief Abstract interface for connecting to real exchanges.
 */
class ExchangeGateway {
public:
    virtual ~ExchangeGateway() = default;

    virtual void connect() = 0;
    virtual void subscribe_market_data(const std::string& symbol) = 0;
    virtual void send_order(const Order& order) = 0;
};

/**
 * @brief Implementation for Binance (REST/WS).
 */
class BinanceAdapter : public ExchangeGateway {
public:
    void connect() override {
        ARGENTUM_LOG(INFO, "[Gateway] Connecting to Binance...");
    }

    void subscribe_market_data(const std::string& symbol) override {
        ARGENTUM_LOG(INFO, "[Gateway] Subscribed to " << symbol << " on Binance.");
    }

    void send_order(const Order& order) override {
        ARGENTUM_LOG(INFO, "[Gateway] Order " << order.order_id << " sent to Binance.");
    }
};

} // namespace argentum::gateway
