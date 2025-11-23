#include "strategy/orderbook_manager.hpp"
#include "mexc/spot_client.hpp"

#include <nlohmann/json.hpp>
#include <iostream>

namespace strategy {

OrderBookManager::OrderBookManager(const std::string& symbol)
    : symbol_(symbol),
      orderbook_(symbol) {
}

bool OrderBookManager::subscribe(mexc::WsSpotClient& ws_client, mexc::SpotClient* rest_client) {
    if (subscribed_) {
        return true;
    }

    // For aggregated depth, we need an initial snapshot from REST API
    // because WebSocket only sends incremental updates (changes)
    if (rest_client) {
        try {
            std::cout << "[OrderBook] Fetching initial snapshot from REST API..." << std::endl;
            std::string depth_response = rest_client->depth(symbol_, 100); // Get 100 levels for full orderbook
            auto depth_json = nlohmann::json::parse(depth_response);
            
            if (depth_json.contains("bids") && depth_json.contains("asks")) {
                std::vector<PriceLevel> bids = parse_depth_levels(depth_json["bids"]);
                std::vector<PriceLevel> asks = parse_depth_levels(depth_json["asks"]);
                long long last_update_id = depth_json.value("lastUpdateId", 0LL);
                
                if (!bids.empty() && !asks.empty()) {
                    orderbook_.apply_snapshot(bids, asks, last_update_id);
                    std::cout << "[OrderBook] Initial snapshot loaded - bids: " << bids.size() 
                              << ", asks: " << asks.size() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[OrderBook] Failed to fetch initial snapshot: " << e.what() << std::endl;
            // Continue anyway - will try to build from incremental updates
        }
    }

    // Set up depth callback
    ws_client.set_depth_callback([this](const nlohmann::json& data) {
        handle_depth_message(data);
    });

    // Subscribe to depth stream
    bool success = ws_client.subscribe_depth(symbol_, 20); // 20 levels
    
    if (success) {
        subscribed_ = true;
    }
    
    return success;
}

void OrderBookManager::unsubscribe(mexc::WsSpotClient& ws_client) {
    if (subscribed_) {
        ws_client.unsubscribe_depth(symbol_);
        subscribed_ = false;
    }
}

bool OrderBookManager::handle_depth_message(const nlohmann::json& message) {
    try {
        // MEXC v3 WebSocket depth message format: {"c":"spot@public.depth.v3.api@BTCUSDT","d":{...}}
        // The "d" field contains the actual depth data
        nlohmann::json depth_data;
        long long update_id = 0;
        bool is_snapshot = false;
        
        // MEXC v3 format: {"c": "channel", "d": {...data...}}
        if (message.contains("d") && message.contains("c")) {
            depth_data = message["d"];
            std::string channel = message["c"].get<std::string>();
            
            // For aggregated depth, messages are incremental updates
            // They may contain only bids, only asks, or both
            // We need to accumulate them until we have both sides
            // Treat as snapshot only if orderbook is empty AND we have both bids and asks
            bool has_bids = depth_data.contains("bids") && depth_data["bids"].is_array() && !depth_data["bids"].empty();
            bool has_asks = depth_data.contains("asks") && depth_data["asks"].is_array() && !depth_data["asks"].empty();
            
            // Only treat as snapshot if orderbook is empty AND we have both sides
            // Otherwise, treat as incremental update
            is_snapshot = (orderbook_.last_update_id() == 0 && has_bids && has_asks);
        } else if (message.contains("data") && message.contains("channel")) {
            // Alternative format
            depth_data = message["data"];
            std::string channel = message["channel"].get<std::string>();
            is_snapshot = (channel.find("depth") != std::string::npos && 
                          (channel.find("snapshot") != std::string::npos ||
                           orderbook_.last_update_id() == 0));
            if (message.contains("ts")) {
                update_id = message["ts"].get<long long>();
            }
        } else {
            // Assume direct depth data (fallback)
            depth_data = message;
        }
        
        // Extract update ID
        if (depth_data.contains("version") || depth_data.contains("lastUpdateId")) {
            update_id = depth_data.contains("version") 
                ? depth_data["version"].get<long long>()
                : depth_data["lastUpdateId"].get<long long>();
        } else if (message.contains("version") || message.contains("lastUpdateId")) {
            update_id = message.contains("version")
                ? message["version"].get<long long>()
                : message["lastUpdateId"].get<long long>();
        }
        
        // Parse bids and asks
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
        
        if (depth_data.contains("bids") && depth_data["bids"].is_array()) {
            bids = parse_depth_levels(depth_data["bids"]);
        }
        
        if (depth_data.contains("asks") && depth_data["asks"].is_array()) {
            asks = parse_depth_levels(depth_data["asks"]);
        }
        
        if (bids.empty() && asks.empty()) {
            return false;
        }
        
        // Apply update
        // For aggregated depth, MEXC sends incremental updates (only changes)
        // Each message may contain:
        // - Only bids: changes to bids side (merge, don't replace)
        // - Only asks: changes to asks side (merge, don't replace)
        // - Both: changes to both sides
        // We merge these incremental updates into the existing orderbook
        if (is_snapshot) {
            // Full snapshot with both bids and asks (from REST API)
            orderbook_.apply_snapshot(bids, asks, update_id);
        } else {
            // Incremental update - merge changes into existing orderbook
            orderbook_.apply_update(bids, asks, update_id);
        }
        
        // Notify callback
        if (update_callback_) {
            auto snapshot = orderbook_.get_snapshot(5, false);
            update_callback_(snapshot);
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[OrderBook] Failed to handle depth message: " << e.what() << std::endl;
        return false;
    }
}

std::vector<PriceLevel> OrderBookManager::parse_depth_levels(const nlohmann::json& levels) {
    std::vector<PriceLevel> result;
    
    if (!levels.is_array()) {
        return result;
    }
    
    result.reserve(levels.size());
    
    for (const auto& level : levels) {
        if (!level.is_array() || level.size() < 2) {
            continue;
        }
        
        try {
            double price = 0.0;
            double quantity = 0.0;
            
            // Handle string or number format
            if (level[0].is_string()) {
                price = std::stod(level[0].get<std::string>());
            } else {
                price = level[0].get<double>();
            }
            
            if (level[1].is_string()) {
                quantity = std::stod(level[1].get<std::string>());
            } else {
                quantity = level[1].get<double>();
            }
            
            if (price > 0.0 && quantity > 0.0) {
                result.emplace_back(price, quantity);
            }
        } catch (const std::exception&) {
            // Skip invalid levels
            continue;
        }
    }
    
    return result;
}

void OrderBookManager::parse_depth_update(const nlohmann::json& message,
                                         std::vector<PriceLevel>& bids,
                                         std::vector<PriceLevel>& asks,
                                         long long& update_id) {
    // Parse incremental update format
    // This is typically used for real-time updates
    if (message.contains("bids")) {
        bids = parse_depth_levels(message["bids"]);
    }
    if (message.contains("asks")) {
        asks = parse_depth_levels(message["asks"]);
    }
    if (message.contains("version") || message.contains("lastUpdateId")) {
        update_id = message.contains("version")
            ? message["version"].get<long long>()
            : message["lastUpdateId"].get<long long>();
    }
}

void OrderBookManager::set_update_callback(UpdateCallback callback) {
    update_callback_ = std::move(callback);
}

} // namespace strategy

