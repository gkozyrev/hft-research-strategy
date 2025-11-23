#pragma once

#include "strategy/orderbook.hpp"
#include "strategy/latency_tracker.hpp"
#include "mexc/ws_spot_client.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <functional>

namespace strategy {

// Manages order book updates from WebSocket depth stream
class OrderBookManager {
public:
    explicit OrderBookManager(const std::string& symbol);
    ~OrderBookManager() = default;

    OrderBookManager(const OrderBookManager&) = delete;
    OrderBookManager& operator=(const OrderBookManager&) = delete;
    OrderBookManager(OrderBookManager&&) = delete;
    OrderBookManager& operator=(OrderBookManager&&) = delete;

    // Get the order book instance
    [[nodiscard]] OrderBook& get_orderbook() { return orderbook_; }
    [[nodiscard]] const OrderBook& get_orderbook() const { return orderbook_; }

    // Subscribe to depth updates via WebSocket
    // Optionally provide REST client to fetch initial snapshot
    bool subscribe(mexc::WsSpotClient& ws_client, mexc::SpotClient* rest_client = nullptr);

    // Unsubscribe from depth updates
    void unsubscribe(mexc::WsSpotClient& ws_client);

    // Parse and apply MEXC depth message (JSON format)
    // Returns true if update was applied successfully
    bool handle_depth_message(const nlohmann::json& message);

    // Set callback for when order book is updated
    using UpdateCallback = std::function<void(const OrderBookSnapshot&)>;
    void set_update_callback(UpdateCallback callback);
    
    // Get latency tracker
    [[nodiscard]] const LatencyTracker& get_latency_tracker() const { return latency_tracker_; }
    [[nodiscard]] LatencyTracker& get_latency_tracker() { return latency_tracker_; }

private:
    std::string symbol_;
    OrderBook orderbook_;
    UpdateCallback update_callback_;
    bool subscribed_ = false;
    LatencyTracker latency_tracker_; // Tracks end-to-end latency from message received to orderbook updated
    
    // Version tracking for aggregated depth
    std::string last_to_version_; // Last toVersion received from WebSocket
    long long snapshot_version_ = 0; // Version from initial REST API snapshot
    
    // Parse MEXC depth snapshot format
    std::vector<PriceLevel> parse_depth_levels(const nlohmann::json& levels);
    
    // Parse MEXC depth update format
    void parse_depth_update(const nlohmann::json& message,
                           std::vector<PriceLevel>& bids,
                           std::vector<PriceLevel>& asks,
                           long long& update_id);
};

} // namespace strategy

