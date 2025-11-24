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
                    
                    // Verify snapshot was applied correctly
                    bool is_valid = orderbook_.is_valid();
                    double best_bid = 0.0, best_ask = 0.0;
                    try {
                        best_bid = orderbook_.best_bid();
                        best_ask = orderbook_.best_ask();
                    } catch (...) {
                        // Orderbook might be invalid
                    }
                    
                    std::cout << "[OrderBook] Initial snapshot loaded - bids: " << bids.size() 
                              << ", asks: " << asks.size() 
                              << ", version: " << snapshot_version_
                              << ", orderbook valid: " << is_valid
                              << ", best_bid: " << best_bid
                              << ", best_ask: " << best_ask
                              << ", spread: " << (best_ask - best_bid) << std::endl;
                    
                    if (!is_valid) {
                        std::cerr << "[OrderBook] WARNING: Snapshot resulted in invalid orderbook! "
                                  << "bid_levels: " << bids.size() << ", ask_levels: " << asks.size()
                                  << ", best_bid: " << best_bid << ", best_ask: " << best_ask << std::endl;
                    }
                } else {
                    std::cerr << "[OrderBook] WARNING: Snapshot has empty bids or asks - bids: " 
                              << bids.size() << ", asks: " << asks.size() << std::endl;
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
    
    // Helper lambda to get latency in microseconds
    auto get_latency_us = [&start_time]() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count();
    };
    
    try {
        // DEBUG: Log raw message received
        std::cerr << "[DEBUG] Message received (latency: " << get_latency_us() << "us) - orderbook last_update_id=" 
                  << orderbook_.last_update_id() 
                  << ", last_to_version=" << (last_to_version_.empty() ? "none" : last_to_version_)
                  << ", snapshot_version=" << snapshot_version_ << std::endl;
        
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
            std::cerr << "[DEBUG] Message skipped (latency: " << get_latency_us() << "us): empty bids and asks" << std::endl;
            return false;
        }
        
        std::cerr << "[DEBUG] Parsed message (latency: " << get_latency_us() << "us) - bids: " << bids.size() 
                  << ", asks: " << asks.size() << ", is_snapshot: " << is_snapshot << std::endl;
        
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
        
        std::cerr << "[DEBUG] Version info (latency: " << get_latency_us() << "us) - fromVersion: " << (from_version.empty() ? "none" : from_version)
                  << ", toVersion: " << (to_version.empty() ? "none" : to_version) << std::endl;
        
        // Extract update ID - for aggregated depth, use toVersion as the update_id
        // This represents the highest version included in this update message
        // Note: fromVersion is the starting version, toVersion is the ending version
        // We use toVersion because it represents the state after applying all changes in this message
        if (!to_version.empty()) {
            try {
                update_id = std::stoll(to_version);
                std::cerr << "[DEBUG] Using toVersion as update_id (latency: " << get_latency_us() << "us): " << update_id << std::endl;
            } catch (...) {
                // If toVersion is not numeric, fall through to other options
                std::cerr << "[DEBUG] toVersion is not numeric (latency: " << get_latency_us() << "us): " << to_version << std::endl;
            }
        }
        
        // If we have fromVersion but no toVersion, use fromVersion as update_id
        // (This handles edge cases where toVersion might be missing)
        if (update_id == 0 && !from_version.empty()) {
            try {
                update_id = std::stoll(from_version);
                std::cerr << "[DEBUG] Using fromVersion as update_id (latency: " << get_latency_us() << "us): " << update_id << std::endl;
            } catch (...) {
                // Ignore parse errors
            }
        }
        
        if (update_id == 0) {
            // Fallback to other version fields
            if (depth_data.contains("version") || depth_data.contains("lastUpdateId")) {
                update_id = depth_data.contains("version") 
                    ? depth_data["version"].get<long long>()
                    : depth_data["lastUpdateId"].get<long long>();
                std::cerr << "[DEBUG] Using fallback version field as update_id (latency: " << get_latency_us() << "us): " << update_id << std::endl;
            } else if (message.contains("version") || message.contains("lastUpdateId")) {
                update_id = message.contains("version")
                    ? message["version"].get<long long>()
                    : message["lastUpdateId"].get<long long>();
                std::cerr << "[DEBUG] Using message-level version field as update_id (latency: " << get_latency_us() << "us): " << update_id << std::endl;
            } else {
                std::cerr << "[DEBUG] WARNING: No update_id found in message! (latency: " << get_latency_us() << "us)" << std::endl;
            }
        }
        
        // Check version continuity for aggregated depth
        if (!from_version.empty() && !to_version.empty()) {
            std::cerr << "[DEBUG] Checking version continuity (latency: " << get_latency_us() << "us)..." << std::endl;
            if (!last_to_version_.empty()) {
                // Calculate expected fromVersion (last toVersion + 1)
                try {
                    long long last_version = std::stoll(last_to_version_);
                    long long expected_from = last_version + 1;
                    long long actual_from = std::stoll(from_version);
                    
                    std::cerr << "[DEBUG] Version check (latency: " << get_latency_us() << "us) - last_to_version: " << last_version 
                              << ", expected fromVersion: " << expected_from 
                              << ", actual fromVersion: " << actual_from << std::endl;
                    
                    if (actual_from != expected_from) {
                        long long gap = actual_from - expected_from;
                        std::cerr << "[OrderBook] WARNING: Version gap detected! Expected fromVersion=" << expected_from
                                  << " but got " << actual_from << ". Missing " << gap 
                                  << " updates. Orderbook last_update_id=" << orderbook_.last_update_id()
                                  << " (last_to_version=" << last_to_version_ << "). Orderbook may be stale." << std::endl;
                        // Continue processing - small gaps (1-100) are acceptable due to network timing
                        // Larger gaps will be handled by subsequent gap checks
                    } else {
                        std::cerr << "[DEBUG] Version continuity OK (latency: " << get_latency_us() << "us)" << std::endl;
                    }
                } catch (const std::exception& e) {
                    // Version strings might not be numeric, skip validation
                    std::cerr << "[DEBUG] Version parse error (latency: " << get_latency_us() << "us): " << e.what() << std::endl;
                }
            } else {
                std::cerr << "[DEBUG] No last_to_version set yet (first message after snapshot) (latency: " << get_latency_us() << "us)" << std::endl;
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
                    
                    std::cerr << "[DEBUG] Gap check (latency: " << get_latency_us() << "us) - gap: " << gap << ", fromVersion: " << from_version_num 
                              << ", expected: " << expected_from << std::endl;
                    
                    if (gap > 100) {
                        // Large gap in continuity - adjust baseline instead of rejecting
                        // This prevents the orderbook from getting stuck when there are network issues
                        std::cerr << "[OrderBook] WARNING: Large version gap (" << gap 
                                  << " updates). Adjusting baseline from " << expected_from 
                                  << " to " << from_version_num << ". Orderbook may be stale." << std::endl;
                        // Update last_to_version_ to accept this message and future ones
                        // Set it to from_version_num - 1 so the next check will expect from_version_num
                        last_to_version_ = std::to_string(from_version_num - 1);
                        std::cerr << "[DEBUG] Adjusted last_to_version to (latency: " << get_latency_us() << "us): " << last_to_version_ << std::endl;
                        // Continue processing the message
                    } else if (gap < 0 && -gap > 100) {
                        // Significantly outdated
                        std::cerr << "[OrderBook] Outdated message: fromVersion (" << from_version_num 
                                  << ") is " << -gap << " updates behind expected (" << expected_from 
                                  << "). Ignoring." << std::endl;
                        std::cerr << "[DEBUG] Message rejected (latency: " << get_latency_us() << "us): too outdated" << std::endl;
                        return false;
                    } else {
                        std::cerr << "[DEBUG] Gap acceptable (" << gap << ", latency: " << get_latency_us() << "us), continuing" << std::endl;
                    }
                    // Small gaps (1-100) are acceptable due to network timing
                }
            } catch (const std::exception& e) {
                // Skip if version is not numeric
                std::cerr << "[DEBUG] Version validation error (latency: " << get_latency_us() << "us): " << e.what() << std::endl;
            }
        }
        
        if (is_snapshot) {
            // Full snapshot with both bids and asks (from REST API)
            std::cerr << "[DEBUG] Applying snapshot (latency: " << get_latency_us() << "us) - update_id: " << update_id 
                      << ", bids: " << bids.size() << ", asks: " << asks.size() << std::endl;
            orderbook_.apply_snapshot(bids, asks, update_id);
            snapshot_version_ = update_id;
            
            // Verify snapshot was applied correctly
            bool is_valid_after = orderbook_.is_valid();
            double best_bid_after = 0.0, best_ask_after = 0.0;
            try {
                best_bid_after = orderbook_.best_bid();
                best_ask_after = orderbook_.best_ask();
            } catch (...) {
                // Orderbook might be invalid
            }
            
            std::cerr << "[DEBUG] Snapshot applied (latency: " << get_latency_us() << "us) - snapshot_version now: " << snapshot_version_
                      << ", orderbook valid: " << is_valid_after
                      << ", best_bid: " << best_bid_after
                      << ", best_ask: " << best_ask_after
                      << ", spread: " << (best_ask_after - best_bid_after) << std::endl;
        } else {
            // Incremental update - update absolute quantities at specified price levels
            // Note: These are absolute values, not deltas. Levels not in the message remain unchanged.
            // For aggregated depth, messages may contain only bids, only asks, or both
            // Only apply if orderbook is already valid, or if this message has both sides
            bool orderbook_valid = orderbook_.is_valid();
            // Always get actual values, even if invalid, to see what's wrong
            double best_bid_before = orderbook_.best_bid();
            double best_ask_before = orderbook_.best_ask();
            size_t bid_count = 0, ask_count = 0;
            try {
                auto bids_sample = orderbook_.get_bids(1);
                auto asks_sample = orderbook_.get_asks(1);
                bid_count = bids_sample.size();
                ask_count = asks_sample.size();
            } catch (...) {}
            
            std::cerr << "[DEBUG] Incremental update (latency: " << get_latency_us() << "us) - orderbook valid: " << orderbook_valid 
                      << ", has_bids: " << !bids.empty() << ", has_asks: " << !asks.empty()
                      << ", best_bid_before: " << best_bid_before << ", best_ask_before: " << best_ask_before
                      << ", bid_levels: " << bid_count << ", ask_levels: " << ask_count
                      << ", spread_before: " << (best_ask_before - best_bid_before) 
                      << " (invalid: " << (best_bid_before >= best_ask_before ? "yes" : "no") << ")" << std::endl;
            
            if (orderbook_valid || (!bids.empty() && !asks.empty())) {
                std::cerr << "[DEBUG] Applying update (latency: " << get_latency_us() << "us) - update_id: " << update_id 
                          << ", bid_updates: " << bids.size() << ", ask_updates: " << asks.size() << std::endl;
                orderbook_.apply_update(bids, asks, update_id);
                std::cerr << "[DEBUG] Update applied (latency: " << get_latency_us() << "us) - orderbook last_update_id now: " << orderbook_.last_update_id() << std::endl;
            } else {
                // Orderbook is not valid yet and this message doesn't have both sides
                // Skip this update - wait for a message with both sides or for orderbook to become valid
                std::cerr << "[DEBUG] Update skipped (latency: " << get_latency_us() << "us): orderbook not valid and message incomplete" << std::endl;
                return false;
            }
        }
        
        // Update last version - ONLY update if we actually applied the update to orderbook
        // This keeps version tracking in sync with actual orderbook state
        // Note: Version tracking happens AFTER we know the update was applied
        // (This code runs after apply_snapshot() or apply_update() succeeded)
        std::string old_last_to_version = last_to_version_;
        if (!to_version.empty()) {
            last_to_version_ = to_version;
        } else if (update_id > 0) {
            // Sync last_to_version_ with the update_id we actually applied
            // This keeps tracking consistent even when messages don't have toVersion
            last_to_version_ = std::to_string(update_id);
        }
        std::cerr << "[DEBUG] Version tracking updated (latency: " << get_latency_us() << "us) - last_to_version: " << old_last_to_version 
                  << " -> " << last_to_version_ << std::endl;
        
        // Record latency (from message received to orderbook updated)
        auto end_time = std::chrono::steady_clock::now();
        auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        
        // Check if orderbook is valid - but still notify callback even if invalid
        // so GUI can show the invalid state and potentially recover
        bool is_valid = orderbook_.is_valid();
        if (!is_valid) {
            // Get detailed info about why it's invalid
            double best_bid = 0.0, best_ask = 0.0;
            size_t bid_count = 0, ask_count = 0;
            try {
                best_bid = orderbook_.best_bid();
                best_ask = orderbook_.best_ask();
                auto bids_sample = orderbook_.get_bids(1);
                auto asks_sample = orderbook_.get_asks(1);
                bid_count = bids_sample.size();
                ask_count = asks_sample.size();
            } catch (...) {
                // Ignore exceptions during diagnostic
            }
            
            std::cerr << "[DEBUG] Orderbook invalid after update (latency: " << get_latency_us() << "us) - "
                      << "best_bid: " << best_bid << ", best_ask: " << best_ask 
                      << ", bid_levels: " << bid_count << ", ask_levels: " << ask_count
                      << " (spread invalid: " << (best_bid >= best_ask ? "yes" : "no")
                      << ", bids_empty: " << (bid_count == 0 ? "yes" : "no")
                      << ", asks_empty: " << (ask_count == 0 ? "yes" : "no") << "), but continuing with callback" << std::endl;
            
            // Still record latency and notify callback even if invalid
            // This allows GUI to show the invalid state and user can see what's wrong
            latency_tracker_.record(start_time, end_time);
            
            // Notify callback with invalid snapshot so GUI can display warning
            // Get snapshot BEFORE copying callback to minimize lock hold time
            auto snapshot = orderbook_.get_snapshot(20, true); // Get full snapshot even if invalid
            
            UpdateCallback callback_copy;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback_copy = update_callback_;
            }
            
            if (callback_copy) {
                std::cerr << "[DEBUG] Calling update callback (invalid orderbook, latency: " << latency_us << "us) - best_bid: " << snapshot.best_bid 
                          << ", best_ask: " << snapshot.best_ask << std::endl;
                callback_copy(snapshot);
            }
            
            // Return true to continue processing (don't block future updates)
            return true;
        }
        
        latency_tracker_.record(start_time, end_time);
        
        std::cerr << "[DEBUG] Message processed successfully - latency: " << latency_us << "us, "
                  << "orderbook last_update_id: " << orderbook_.last_update_id() 
                  << ", best_bid: " << orderbook_.best_bid() << ", best_ask: " << orderbook_.best_ask() << std::endl;
        
        // Notify callback (thread-safe: copy callback while holding lock, then call it)
        UpdateCallback callback_copy;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_copy = update_callback_;
        }
        if (callback_copy) {
            auto snapshot = orderbook_.get_snapshot(20, true);
            std::cerr << "[DEBUG] Calling update callback (latency: " << latency_us << "us) - best_bid: " << snapshot.best_bid 
                      << ", best_ask: " << snapshot.best_ask << std::endl;
            callback_copy(snapshot);
        } else {
            std::cerr << "[DEBUG] No update callback set (latency: " << latency_us << "us)" << std::endl;
        }
        
        return true;
    } catch (const std::exception& e) {
        auto error_latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cerr << "[OrderBook] Failed to handle depth message: " << e.what() << std::endl;
        std::cerr << "[DEBUG] Exception occurred (latency: " << error_latency << "us), message processing failed" << std::endl;
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
    std::lock_guard<std::mutex> lock(callback_mutex_);
    update_callback_ = std::move(callback);
}

} // namespace strategy

