#pragma once

#include "strategy/orderbook.hpp"
#include "strategy/latency_tracker.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace strategy {

// Terminal display for orderbook (CEX-style visualization)
class OrderBookDisplay {
public:
    explicit OrderBookDisplay(const std::string& symbol, int levels = 10);
    ~OrderBookDisplay() = default;

    OrderBookDisplay(const OrderBookDisplay&) = delete;
    OrderBookDisplay& operator=(const OrderBookDisplay&) = delete;
    OrderBookDisplay(OrderBookDisplay&&) = delete;
    OrderBookDisplay& operator=(OrderBookDisplay&&) = delete;

    // Render orderbook to terminal
    void render(const OrderBook& orderbook);
    
    // Render orderbook with latency tracking
    void render(const OrderBook& orderbook, const LatencyTracker& latency_tracker);
    
    // Render with custom snapshot
    void render_snapshot(const OrderBookSnapshot& snapshot);

    // Clear screen and reset cursor
    void clear();

private:
    std::string symbol_;
    int levels_;
    bool first_render_ = true;
    
    // Formatting helpers
    std::string format_price(double price, int precision = 4) const;
    std::string format_quantity(double qty, int precision = 4) const;
    std::string format_volume(double volume) const;
    
    // Color codes (ANSI)
    static constexpr const char* RESET = "\033[0m";
    static constexpr const char* BOLD = "\033[1m";
    static constexpr const char* RED = "\033[31m";
    static constexpr const char* GREEN = "\033[32m";
    static constexpr const char* YELLOW = "\033[33m";
    static constexpr const char* BLUE = "\033[34m";
    static constexpr const char* MAGENTA = "\033[35m";
    static constexpr const char* CYAN = "\033[36m";
    static constexpr const char* GRAY = "\033[90m";
    
    void print_header();
    void print_separator();
    void print_asks(const std::vector<PriceLevel>& asks, double best_ask);
    void print_spread(double best_bid, double best_ask, double spread);
    void print_bids(const std::vector<PriceLevel>& bids, double best_bid);
    void print_stats(const OrderBookSnapshot& snapshot);
    void print_stats(const OrderBookSnapshot& snapshot, const LatencyTracker& latency_tracker);
};

} // namespace strategy

