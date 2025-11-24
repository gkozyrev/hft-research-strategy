#include "strategy/orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <unordered_set>

namespace strategy {

constexpr double kEpsilon = 1e-9;

OrderBook::OrderBook(const std::string& symbol)
    : symbol_(symbol),
      last_update_time_(std::chrono::system_clock::now()) {
}

void OrderBook::apply_snapshot(const std::vector<PriceLevel>& bids,
                              const std::vector<PriceLevel>& asks,
                              long long update_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    bids_.clear();
    asks_.clear();
    
    for (const auto& level : bids) {
        if (level.price > kEpsilon && level.quantity > kEpsilon) {
            bids_[level.price] = level.quantity;
        }
    }
    
    for (const auto& level : asks) {
        if (level.price > kEpsilon && level.quantity > kEpsilon) {
            asks_[level.price] = level.quantity;
        }
    }
    
    last_update_id_ = update_id;
    last_update_time_ = std::chrono::system_clock::now();
}

void OrderBook::apply_update(const std::vector<PriceLevel>& bid_updates,
                             const std::vector<PriceLevel>& ask_updates,
                             long long update_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    update_bids(bid_updates);
    update_asks(ask_updates);
    
    last_update_id_ = update_id;
    last_update_time_ = std::chrono::system_clock::now();
}

void OrderBook::update_bids(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity <= kEpsilon) {
            // Remove level if quantity is zero or negative
            bids_.erase(level.price);
        } else if (level.price > kEpsilon) {
            // Update or insert level
            bids_[level.price] = level.quantity;
        }
    }
}

void OrderBook::update_asks(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity <= kEpsilon) {
            // Remove level if quantity is zero or negative
            asks_.erase(level.price);
        } else if (level.price > kEpsilon) {
            // Update or insert level
            asks_[level.price] = level.quantity;
        }
    }
}

double OrderBook::best_bid() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (bids_.empty()) {
        return 0.0;
    }
    return bids_.begin()->first;
}

double OrderBook::best_ask() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (asks_.empty()) {
        return 0.0;
    }
    return asks_.begin()->first;
}

double OrderBook::spread() const {
    const double bid = best_bid();
    const double ask = best_ask();
    if (bid <= kEpsilon || ask <= kEpsilon || ask <= bid) {
        return 0.0;
    }
    return ask - bid;
}

double OrderBook::microprice(int depth_levels) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return compute_microprice(depth_levels);
}

double OrderBook::compute_microprice(int depth_levels) const {
    if (bids_.empty() || asks_.empty()) {
        return 0.0;
    }
    
    double bid_volume = 0.0;
    double ask_volume = 0.0;
    double bid_weighted_price = 0.0;
    double ask_weighted_price = 0.0;
    
    int count = 0;
    for (const auto& [price, qty] : bids_) {
        if (count >= depth_levels) break;
        const double notional = price * qty;
        bid_volume += notional;
        bid_weighted_price += notional;
        count++;
    }
    
    count = 0;
    for (const auto& [price, qty] : asks_) {
        if (count >= depth_levels) break;
        const double notional = price * qty;
        ask_volume += notional;
        ask_weighted_price += notional;
        count++;
    }
    
    if (bid_volume <= kEpsilon || ask_volume <= kEpsilon) {
        const double mid = (bids_.begin()->first + asks_.begin()->first) * 0.5;
        return mid;
    }
    
    const double total_volume = bid_volume + ask_volume;
    const double bid_contribution = bid_weighted_price / total_volume;
    const double ask_contribution = ask_weighted_price / total_volume;
    
    // Microprice: volume-weighted average of bid and ask sides
    const double best_bid_price = bids_.begin()->first;
    const double best_ask_price = asks_.begin()->first;
    
    return best_bid_price * (ask_volume / total_volume) + 
           best_ask_price * (bid_volume / total_volume);
}

double OrderBook::quantity_at_price(double price, bool is_bid) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (is_bid) {
        const auto it = bids_.find(price);
        return (it != bids_.end()) ? it->second : 0.0;
    } else {
        const auto it = asks_.find(price);
        return (it != asks_.end()) ? it->second : 0.0;
    }
}

double OrderBook::cumulative_volume(bool is_bid, int levels) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    double volume = 0.0;
    int count = 0;
    
    if (is_bid) {
        for (const auto& [price, qty] : bids_) {
            if (count >= levels) break;
            volume += price * qty; // Notional volume
            count++;
        }
    } else {
        for (const auto& [price, qty] : asks_) {
            if (count >= levels) break;
            volume += price * qty; // Notional volume
            count++;
        }
    }
    
    return volume;
}

std::vector<PriceLevel> OrderBook::get_bids(int levels) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<PriceLevel> result;
    result.reserve(std::min(static_cast<size_t>(levels), bids_.size()));
    
    int count = 0;
    for (const auto& [price, qty] : bids_) {
        if (count >= levels) break;
        result.emplace_back(price, qty);
        count++;
    }
    
    return result;
}

std::vector<PriceLevel> OrderBook::get_asks(int levels) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<PriceLevel> result;
    result.reserve(std::min(static_cast<size_t>(levels), asks_.size()));
    
    int count = 0;
    for (const auto& [price, qty] : asks_) {
        if (count >= levels) break;
        result.emplace_back(price, qty);
        count++;
    }
    
    return result;
}

OrderBookSnapshot OrderBook::get_snapshot(int depth_levels, bool include_full_depth) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    OrderBookSnapshot snapshot;
    
    if (bids_.empty() || asks_.empty()) {
        return snapshot;
    }
    
    snapshot.best_bid = bids_.begin()->first;
    snapshot.best_ask = asks_.begin()->first;
    snapshot.spread = snapshot.best_ask - snapshot.best_bid;
    
    // Calculate volumes inline to avoid recursive locking
    double bid_volume = 0.0;
    double ask_volume = 0.0;
    int bid_count = 0;
    int ask_count = 0;
    
    for (const auto& [price, qty] : bids_) {
        if (bid_count >= depth_levels) break;
        bid_volume += price * qty;
        bid_count++;
    }
    
    for (const auto& [price, qty] : asks_) {
        if (ask_count >= depth_levels) break;
        ask_volume += price * qty;
        ask_count++;
    }
    
    snapshot.bid_volume = bid_volume;
    snapshot.ask_volume = ask_volume;
    snapshot.microprice = compute_microprice(depth_levels);
    snapshot.last_update_id = last_update_id_;
    snapshot.timestamp = last_update_time_;
    
    if (include_full_depth) {
        // Inline to avoid recursive locking
        snapshot.bids.reserve(std::min(static_cast<size_t>(depth_levels), bids_.size()));
        snapshot.asks.reserve(std::min(static_cast<size_t>(depth_levels), asks_.size()));
        
        int bid_count = 0;
        for (const auto& [price, qty] : bids_) {
            if (bid_count >= depth_levels) break;
            snapshot.bids.emplace_back(price, qty);
            bid_count++;
        }
        
        int ask_count = 0;
        for (const auto& [price, qty] : asks_) {
            if (ask_count >= depth_levels) break;
            snapshot.asks.emplace_back(price, qty);
            ask_count++;
        }
    }
    
    return snapshot;
}

OrderBookSnapshot OrderBook::get_snapshot_excluding(
    const std::vector<double>& exclude_bid_prices,
    const std::vector<double>& exclude_ask_prices,
    int depth_levels) const {
    
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    OrderBookSnapshot snapshot;
    
    if (bids_.empty() || asks_.empty()) {
        return snapshot;
    }
    
    // Build sets for fast lookup
    std::unordered_set<double> exclude_bids(exclude_bid_prices.begin(), exclude_bid_prices.end());
    std::unordered_set<double> exclude_asks(exclude_ask_prices.begin(), exclude_ask_prices.end());
    
    constexpr double kPriceCompareEps = 1e-6;
    
    // Find best bid excluding own orders
    for (const auto& [price, qty] : bids_) {
        bool should_exclude = false;
        for (double exclude_price : exclude_bids) {
            if (std::fabs(price - exclude_price) <= kPriceCompareEps) {
                should_exclude = true;
                break;
            }
        }
        if (!should_exclude) {
            snapshot.best_bid = price;
            break;
        }
    }
    
    // Find best ask excluding own orders
    for (const auto& [price, qty] : asks_) {
        bool should_exclude = false;
        for (double exclude_price : exclude_asks) {
            if (std::fabs(price - exclude_price) <= kPriceCompareEps) {
                should_exclude = true;
                break;
            }
        }
        if (!should_exclude) {
            snapshot.best_ask = price;
            break;
        }
    }
    
    if (snapshot.best_bid <= kEpsilon || snapshot.best_ask <= kEpsilon) {
        return snapshot;
    }
    
    snapshot.spread = snapshot.best_ask - snapshot.best_bid;
    
    // Calculate volumes excluding own orders
    double bid_volume = 0.0;
    double ask_volume = 0.0;
    int bid_count = 0;
    int ask_count = 0;
    
    for (const auto& [price, qty] : bids_) {
        if (bid_count >= depth_levels) break;
        bool should_exclude = false;
        for (double exclude_price : exclude_bids) {
            if (std::fabs(price - exclude_price) <= kPriceCompareEps) {
                should_exclude = true;
                break;
            }
        }
        if (!should_exclude) {
            bid_volume += price * qty;
            bid_count++;
        }
    }
    
    for (const auto& [price, qty] : asks_) {
        if (ask_count >= depth_levels) break;
        bool should_exclude = false;
        for (double exclude_price : exclude_asks) {
            if (std::fabs(price - exclude_price) <= kPriceCompareEps) {
                should_exclude = true;
                break;
            }
        }
        if (!should_exclude) {
            ask_volume += price * qty;
            ask_count++;
        }
    }
    
    snapshot.bid_volume = bid_volume;
    snapshot.ask_volume = ask_volume;
    
    // Compute microprice excluding own orders
    const double total_volume = bid_volume + ask_volume;
    if (total_volume > kEpsilon) {
        snapshot.microprice = snapshot.best_bid * (ask_volume / total_volume) + 
                             snapshot.best_ask * (bid_volume / total_volume);
    } else {
        snapshot.microprice = (snapshot.best_bid + snapshot.best_ask) * 0.5;
    }
    
    snapshot.last_update_id = last_update_id_;
    snapshot.timestamp = last_update_time_;
    
    return snapshot;
}

bool OrderBook::is_valid() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return !bids_.empty() && !asks_.empty() && 
           bids_.begin()->first < asks_.begin()->first;
}

long long OrderBook::last_update_id() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return last_update_id_;
}

std::chrono::system_clock::time_point OrderBook::last_update_time() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return last_update_time_;
}

void OrderBook::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    bids_.clear();
    asks_.clear();
    last_update_id_ = 0;
    last_update_time_ = std::chrono::system_clock::now();
}

} // namespace strategy

