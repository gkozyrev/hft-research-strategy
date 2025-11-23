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
                    snapshot_version_ = last_update_id;
                    last_to_version_.clear(); // Reset WebSocket version tracking
                    std::cout << "[OrderBook] Initial snapshot loaded - bids: " << bids.size() 
                              << ", asks: " << asks.size() 
                              << ", version: " << snapshot_version_ << std::endl;
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
    auto start_time = std::chrono::steady_clock::now();
    
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
        
        // Parse bids and asks first
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
        
        // Extract version information for aggregated depth
        // For aggregated depth, MEXC uses fromVersion and toVersion
        std::string from_version;
        std::string to_version;
        if (depth_data.contains("fromVersion")) {
            from_version = depth_data["fromVersion"].get<std::string>();
        }
        if (depth_data.contains("toVersion")) {
            to_version = depth_data["toVersion"].get<std::string>();
        }
        
        // Extract update ID - prefer toVersion for aggregated depth, fallback to other fields
        if (!to_version.empty()) {
            try {
                update_id = std::stoll(to_version);
            } catch (...) {
                // If toVersion is not numeric, fall through to other options
            }
        }
        
        if (update_id == 0) {
            // Fallback to other version fields
            if (depth_data.contains("version") || depth_data.contains("lastUpdateId")) {
                update_id = depth_data.contains("version") 
                    ? depth_data["version"].get<long long>()
                    : depth_data["lastUpdateId"].get<long long>();
            } else if (message.contains("version") || message.contains("lastUpdateId")) {
                update_id = message.contains("version")
                    ? message["version"].get<long long>()
                    : message["lastUpdateId"].get<long long>();
            }
        }
        
        // Check version continuity for aggregated depth
        if (!from_version.empty() && !to_version.empty()) {
            if (!last_to_version_.empty()) {
                // Calculate expected fromVersion (last toVersion + 1)
                try {
                    long long last_version = std::stoll(last_to_version_);
                    long long expected_from = last_version + 1;
                    long long actual_from = std::stoll(from_version);
                    
                    if (actual_from != expected_from) {
                        std::cerr << "[OrderBook] WARNING: Version gap detected! Expected fromVersion=" << expected_from
                                  << " but got " << actual_from << ". Missing " << (actual_from - expected_from) 
                                  << " updates. Orderbook may be stale." << std::endl;
                        // For now, continue - but in production you might want to reinitialize
                    }
                } catch (...) {
                    // Version strings might not be numeric, skip validation
                }
            }
            
            // Check version continuity
            // For aggregated depth, we need to ensure fromVersion matches our expected version
            try {
                long long from_version_num = std::stoll(from_version);
                
                if (last_to_version_.empty()) {
                    // First WebSocket message after snapshot - check gap from snapshot
                    if (snapshot_version_ > 0) {
                        long long expected_from = snapshot_version_ + 1;
                        long long gap = from_version_num - expected_from;
                        
                        if (gap > 5000) {
                            // Extremely large gap - skip, would corrupt orderbook
                            std::cerr << "[OrderBook] Skipping first message: fromVersion (" << from_version_num 
                                      << ") is " << gap << " updates ahead of snapshot (" << snapshot_version_ 
                                      << "). Orderbook would be corrupted." << std::endl;
                            return false;
                        } else if (gap > 0) {
                            // Acceptable gap - adjust baseline to accept this and future messages
                            // This is normal in high-frequency markets
                            if (gap > 1000) {
                                std::cerr << "[OrderBook] WARNING: First message gap of " << gap 
                                          << " updates from snapshot. Adjusting baseline." << std::endl;
                            }
                            snapshot_version_ = from_version_num - 1;
                        } else if (gap < 0 && -gap > 100) {
                            // Significantly outdated
                            std::cerr << "[OrderBook] Outdated first message: fromVersion (" << from_version_num 
                                      << ") is " << -gap << " updates behind snapshot. Ignoring." << std::endl;
                            return false;
                        }
                        // If gap == 0 or small negative, that's fine
                    } else {
                        // No snapshot - accept as baseline
                        if (from_version_num > 0) {
                            snapshot_version_ = from_version_num - 1;
                        }
                    }
                } else {
                    // Subsequent messages - check continuity with last_to_version_
                    long long last_version = std::stoll(last_to_version_);
                    long long expected_from = last_version + 1;
                    long long gap = from_version_num - expected_from;
                    
                    if (gap > 100) {
                        // Large gap in continuity - skip
                        std::cerr << "[OrderBook] Skipping message: fromVersion (" << from_version_num 
                                  << ") is " << gap << " updates ahead of expected (" << expected_from 
                                  << "). Missing updates." << std::endl;
                        return false;
                    } else if (gap < 0 && -gap > 100) {
                        // Significantly outdated
                        std::cerr << "[OrderBook] Outdated message: fromVersion (" << from_version_num 
                                  << ") is " << -gap << " updates behind expected (" << expected_from 
                                  << "). Ignoring." << std::endl;
                        return false;
                    }
                    // Small gaps (1-100) are acceptable due to network timing
                }
            } catch (...) {
                // Skip if version is not numeric
            }
        }
        
        if (is_snapshot) {
            // Full snapshot with both bids and asks (from REST API)
            orderbook_.apply_snapshot(bids, asks, update_id);
            snapshot_version_ = update_id;
        } else {
            // Incremental update - update absolute quantities at specified price levels
            // Note: These are absolute values, not deltas. Levels not in the message remain unchanged.
            // For aggregated depth, messages may contain only bids, only asks, or both
            // Only apply if orderbook is already valid, or if this message has both sides
            if (orderbook_.is_valid() || (!bids.empty() && !asks.empty())) {
                orderbook_.apply_update(bids, asks, update_id);
            } else {
                // Orderbook is not valid yet and this message doesn't have both sides
                // Skip this update - wait for a message with both sides or for orderbook to become valid
                return false;
            }
        }
        
        // Update last version
        if (!to_version.empty()) {
            last_to_version_ = to_version;
        }
        
        // Only proceed if orderbook is valid
        if (!orderbook_.is_valid()) {
            return false;
        }
        
        // Record latency (from message received to orderbook updated)
        auto end_time = std::chrono::steady_clock::now();
        latency_tracker_.record(start_time, end_time);
        
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

