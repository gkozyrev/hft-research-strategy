#pragma once

#include <map>
#include <shared_mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace strategy {

// Price level in the order book
struct PriceLevel {
    double price = 0.0;
    double quantity = 0.0;
    
    PriceLevel() = default;
    PriceLevel(double p, double q) : price(p), quantity(q) {}
};

// Order book snapshot for fast queries
struct OrderBookSnapshot {
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread = 0.0;
    double bid_volume = 0.0;      // Cumulative volume up to N levels
    double ask_volume = 0.0;
    double microprice = 0.0;      // Volume-weighted mid price
    std::chrono::system_clock::time_point timestamp;
    long long last_update_id = 0;
    
    // Optional: full depth levels (for advanced queries)
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

// In-memory order book optimized for low-latency HFT operations
class OrderBook {
public:
    explicit OrderBook(const std::string& symbol);
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Update methods (from WebSocket depth stream)
    // Apply full snapshot (replaces entire order book)
    void apply_snapshot(const std::vector<PriceLevel>& bids,
                       const std::vector<PriceLevel>& asks,
                       long long update_id);
    
    // Apply incremental update (add/update/remove price levels)
    void apply_update(const std::vector<PriceLevel>& bid_updates,
                     const std::vector<PriceLevel>& ask_updates,
                     long long update_id);

    // Query methods (fast, lock-free where possible)
    [[nodiscard]] double best_bid() const;
    [[nodiscard]] double best_ask() const;
    [[nodiscard]] double spread() const;
    [[nodiscard]] double microprice(int depth_levels = 5) const;
    
    // Get quantity at specific price level
    [[nodiscard]] double quantity_at_price(double price, bool is_bid) const;
    
    // Get cumulative volume up to N levels
    [[nodiscard]] double cumulative_volume(bool is_bid, int levels = 5) const;
    
    // Get price levels (top N)
    [[nodiscard]] std::vector<PriceLevel> get_bids(int levels = 10) const;
    [[nodiscard]] std::vector<PriceLevel> get_asks(int levels = 10) const;
    
    // Get full snapshot (for compatibility with existing code)
    [[nodiscard]] OrderBookSnapshot get_snapshot(int depth_levels = 5, 
                                                 bool include_full_depth = false) const;
    
    // Get snapshot excluding own orders at specific prices
    [[nodiscard]] OrderBookSnapshot get_snapshot_excluding(
        const std::vector<double>& exclude_bid_prices,
        const std::vector<double>& exclude_ask_prices,
        int depth_levels = 5) const;
    
    // Check if order book is valid (has both bid and ask)
    [[nodiscard]] bool is_valid() const;
    
    // Get last update ID
    [[nodiscard]] long long last_update_id() const;
    
    // Get timestamp of last update
    [[nodiscard]] std::chrono::system_clock::time_point last_update_time() const;
    
    // Clear order book
    void clear();

private:
    // Price level storage: map<price, quantity>
    // Bids: descending order (highest price first)
    // Asks: ascending order (lowest price first)
    using BidMap = std::map<double, double, std::greater<double>>;
    using AskMap = std::map<double, double>;
    
    std::string symbol_;
    BidMap bids_;
    AskMap asks_;
    
    mutable std::shared_mutex mutex_; // Reader-writer lock for better concurrency
    long long last_update_id_ = 0;
    std::chrono::system_clock::time_point last_update_time_;
    
    // Helper methods
    void update_bids(const std::vector<PriceLevel>& updates);
    void update_asks(const std::vector<PriceLevel>& updates);
    double compute_microprice(int depth_levels) const;
};

} // namespace strategy

