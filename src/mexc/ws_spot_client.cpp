#include "mexc/ws_spot_client.hpp"
#include "mexc/spot_client.hpp"
#include "mexc/util.hpp"

#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <algorithm>

// Protobuf generated headers
#include "PushDataV3ApiWrapper.pb.h"
#include "PublicAggreDepthsV3Api.pb.h"

namespace mexc {

WsSpotClient::WsSpotClient(Credentials credentials, const std::string& base_ws_url, SpotClient* rest_client)
    : credentials_(std::move(credentials)),
      base_ws_url_(base_ws_url),
      rest_client_(rest_client),
      auto_reconnect_(true),
      max_reconnect_attempts_(-1),
      reconnect_delay_ms_(1000) {
    
    // Initialize public WebSocket client for market data
    public_ws_ = std::make_unique<WsClient>(base_ws_url_);
    
    // Set up message handler
    public_ws_->set_message_callback([this](const std::string& message) {
        // Only log subscription confirmations, not every message (to avoid corrupting display)
        if (message.find("\"code\":0") != std::string::npos && message.find("msg") != std::string::npos) {
            std::cout << "[WS] Subscription confirmed" << std::endl;
        }
        handle_message(message);
    });
    
    // Set up binary message handler for Protobuf messages
    public_ws_->set_binary_callback([this](const std::vector<uint8_t>& data) {
        handle_binary_message(data);
    });
    
    public_ws_->set_error_callback([this](const std::string& error) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (error_callback_) {
            error_callback_(error);
        }
    });
    
    public_ws_->set_state_callback([this](WsConnectionState state) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (state_callback_) {
            state_callback_(state);
        }
        
        if (state == WsConnectionState::Connected) {
            // Resubscribe to all active subscriptions
            resubscribe_all();
        }
    });
    
    public_ws_->set_auto_reconnect(true);
}

WsSpotClient::~WsSpotClient() {
    disconnect();
}

bool WsSpotClient::connect() {
    return public_ws_->connect();
}

void WsSpotClient::disconnect() {
    public_ws_->disconnect();
    if (user_ws_) {
        user_ws_->disconnect();
    }
}

bool WsSpotClient::is_connected() const {
    return public_ws_->is_connected();
}

bool WsSpotClient::subscribe_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.ticker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::unsubscribe_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.ticker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_depth(const std::string& symbol, int limit) {
    Subscription sub;
    sub.method = "sub.depth";
    sub.symbol = to_upper_copy(symbol);
    sub.limit = limit;
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    std::cout << "[WS] Sending depth subscription: " << msg << std::endl;
    bool sent = public_ws_->send(msg);
    if (!sent) {
        std::cerr << "[WS] Failed to send subscription message (connected: " 
                  << public_ws_->is_connected() << ")" << std::endl;
    }
    return sent;
}

bool WsSpotClient::unsubscribe_depth(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.depth";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_trades(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.trades";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::unsubscribe_trades(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.trades";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_klines(const std::string& symbol, const std::string& interval) {
    Subscription sub;
    sub.method = "sub.kline";
    sub.symbol = to_upper_copy(symbol);
    sub.interval = interval;
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::unsubscribe_klines(const std::string& symbol, const std::string& interval) {
    Subscription sub;
    sub.method = "sub.kline";
    sub.symbol = to_upper_copy(symbol);
    sub.interval = interval;
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol && s.interval == sub.interval;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_book_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.bookTicker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::unsubscribe_book_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.bookTicker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_mini_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.miniTicker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.push_back(sub);
    
    std::string msg = build_subscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::unsubscribe_mini_ticker(const std::string& symbol) {
    Subscription sub;
    sub.method = "sub.miniTicker";
    sub.symbol = to_upper_copy(symbol);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    active_subscriptions_.erase(
        std::remove_if(active_subscriptions_.begin(), active_subscriptions_.end(),
            [&sub](const Subscription& s) {
                return s.method == sub.method && s.symbol == sub.symbol;
            }),
        active_subscriptions_.end());
    
    std::string msg = build_unsubscribe_message(sub);
    return public_ws_->send(msg);
}

bool WsSpotClient::subscribe_user_data() {
    if (!rest_client_) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (error_callback_) {
            error_callback_("REST client required for user data streams");
        }
        return false;
    }

    try {
        // Get listen key from REST API
        std::string response = rest_client_->create_listen_key();
        auto json = nlohmann::json::parse(response);
        
        if (json.contains("listenKey")) {
            std::lock_guard<std::mutex> lock(listen_key_mutex_);
            listen_key_ = json["listenKey"].get<std::string>();
        } else {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (error_callback_) {
                error_callback_("Failed to get listen key from REST API");
            }
            return false;
        }

        // Connect to user data stream
        std::string user_ws_url = base_ws_url_ + "?listenKey=" + listen_key_;
        user_ws_ = std::make_unique<WsClient>(user_ws_url);
        
        user_ws_->set_message_callback([this](const std::string& message) {
            handle_message(message);
        });
        
        user_ws_->set_error_callback([this](const std::string& error) {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (error_callback_) {
                error_callback_(error);
            }
        });
        
        user_ws_->set_state_callback([this](WsConnectionState state) {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (state_callback_) {
                state_callback_(state);
            }
        });
        
        user_ws_->set_auto_reconnect(true);
        return user_ws_->connect();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (error_callback_) {
            error_callback_(std::string("Failed to subscribe to user data: ") + e.what());
        }
        return false;
    }
}

void WsSpotClient::unsubscribe_user_data() {
    if (user_ws_) {
        user_ws_->disconnect();
        user_ws_.reset();
    }
    
    // Delete listen key
    if (rest_client_ && !listen_key_.empty()) {
        try {
            rest_client_->delete_listen_key(listen_key_);
        } catch (...) {
            // Ignore errors when deleting listen key
        }
    }
    
    std::lock_guard<std::mutex> lock(listen_key_mutex_);
    listen_key_.clear();
}

bool WsSpotClient::refresh_listen_key() {
    if (!rest_client_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(listen_key_mutex_);
    if (listen_key_.empty()) {
        return false;
    }
    
    try {
        rest_client_->extend_listen_key(listen_key_);
        return true;
    } catch (...) {
        return false;
    }
}

void WsSpotClient::set_ticker_callback(TickerCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    ticker_callback_ = std::move(callback);
}

void WsSpotClient::set_depth_callback(DepthCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    depth_callback_ = std::move(callback);
}

void WsSpotClient::set_trade_callback(TradeCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    trade_callback_ = std::move(callback);
}

void WsSpotClient::set_kline_callback(KlineCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    kline_callback_ = std::move(callback);
}

void WsSpotClient::set_book_ticker_callback(BookTickerCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    book_ticker_callback_ = std::move(callback);
}

void WsSpotClient::set_account_update_callback(AccountUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    account_update_callback_ = std::move(callback);
}

void WsSpotClient::set_order_update_callback(OrderUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    order_update_callback_ = std::move(callback);
}

void WsSpotClient::set_balance_update_callback(BalanceUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    balance_update_callback_ = std::move(callback);
}

void WsSpotClient::set_error_callback(WsErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    error_callback_ = std::move(callback);
}

void WsSpotClient::set_state_callback(WsStateCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    state_callback_ = std::move(callback);
}

void WsSpotClient::set_auto_reconnect(bool enable, int max_reconnect_attempts) {
    auto_reconnect_ = enable;
    max_reconnect_attempts_ = max_reconnect_attempts;
    public_ws_->set_auto_reconnect(enable, max_reconnect_attempts);
}

void WsSpotClient::set_reconnect_delay_ms(int delay_ms) {
    reconnect_delay_ms_ = delay_ms;
    public_ws_->set_reconnect_delay_ms(delay_ms);
}

void WsSpotClient::handle_binary_message(const std::vector<uint8_t>& data) {
    // Handle Protobuf-encoded binary messages
    // For aggregated depth channel: spot@public.aggre.depth.v3.api.pb
    
    try {
        // Deserialize the wrapper message
        PushDataV3ApiWrapper wrapper;
        if (!wrapper.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (error_callback_) {
                error_callback_("Failed to parse Protobuf message");
            }
            return;
        }
        
        // Check if this is aggregated depth data
        if (wrapper.has_publicaggredepths()) {
            const auto& depth_data = wrapper.publicaggredepths();
            
            // Convert Protobuf message to JSON format for orderbook manager
            nlohmann::json json_data;
            json_data["c"] = wrapper.channel();
            if (wrapper.has_symbol()) {
                json_data["symbol"] = wrapper.symbol();
            }
            
            // Convert bids and asks to JSON arrays
            nlohmann::json bids = nlohmann::json::array();
            for (int i = 0; i < depth_data.bids_size(); ++i) {
                const auto& item = depth_data.bids(i);
                std::string price_str = item.price();
                std::string qty_str = item.quantity();
                // Ensure we have valid price and quantity strings
                if (!price_str.empty() && !qty_str.empty()) {
                    bids.push_back(nlohmann::json::array({price_str, qty_str}));
                }
            }
            
            nlohmann::json asks = nlohmann::json::array();
            for (int i = 0; i < depth_data.asks_size(); ++i) {
                const auto& item = depth_data.asks(i);
                std::string price_str = item.price();
                std::string qty_str = item.quantity();
                // Ensure we have valid price and quantity strings
                if (!price_str.empty() && !qty_str.empty()) {
                    asks.push_back(nlohmann::json::array({price_str, qty_str}));
                }
            }
            
            nlohmann::json depth_json;
            depth_json["bids"] = bids;
            depth_json["asks"] = asks;
            depth_json["eventType"] = depth_data.eventtype();
            if (!depth_data.fromversion().empty()) {
                depth_json["fromVersion"] = depth_data.fromversion();
            }
            if (!depth_data.toversion().empty()) {
                depth_json["toVersion"] = depth_data.toversion();
            }
            
            json_data["d"] = depth_json;
            
            // Call the depth callback with the JSON data
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (depth_callback_) {
                depth_callback_(json_data);
            }
        } else {
            // Other message types - log for now
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            std::cout << "[WS] Received Protobuf message with channel: " << wrapper.channel() 
                      << " (not aggregated depth, skipping)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (error_callback_) {
            error_callback_("Error processing Protobuf message: " + std::string(e.what()));
        }
    }
}

void WsSpotClient::handle_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        // Check if it's a market data message or user data message
        if (json.contains("c") || json.contains("channel")) {
            handle_market_data(json);
        } else if (json.contains("e")) {
            handle_user_data(json);
        } else if (json.contains("stream")) {
            // Handle stream format
            if (json["stream"].is_string()) {
                std::string stream = json["stream"];
                if (json.contains("data")) {
                    handle_market_data(json["data"]);
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (error_callback_) {
            error_callback_("JSON parse error: " + std::string(e.what()));
        }
    }
}

void WsSpotClient::handle_market_data(const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    
    // MEXC v3 format: {"c": "channel", "d": {...data...}}
    nlohmann::json actual_data = data;
    std::string channel;
    
    if (data.contains("c")) {
        channel = data["c"].get<std::string>();
        if (data.contains("d")) {
            actual_data = data["d"];
        }
    } else if (data.contains("channel")) {
        channel = data["channel"].get<std::string>();
        if (data.contains("data")) {
            actual_data = data["data"];
        }
    }
    
    std::cout << "[WS] Handling market data, channel: " << channel << std::endl;
    
    if (channel.find("ticker") != std::string::npos) {
        if (ticker_callback_) {
            ticker_callback_(actual_data);
        }
    } else if (channel.find("depth") != std::string::npos) {
        std::cout << "[WS] Depth message received, calling depth callback" << std::endl;
        if (depth_callback_) {
            depth_callback_(actual_data);
        } else {
            std::cout << "[WS] WARNING: Depth callback not set!" << std::endl;
        }
    } else if (channel.find("deals") != std::string::npos || channel.find("trade") != std::string::npos) {
        if (trade_callback_) {
            trade_callback_(actual_data);
        }
    } else if (channel.find("kline") != std::string::npos) {
        if (kline_callback_) {
            kline_callback_(actual_data);
        }
    } else if (channel.find("bookTicker") != std::string::npos) {
        if (book_ticker_callback_) {
            book_ticker_callback_(actual_data);
        }
    } else {
        std::cout << "[WS] Unknown channel type: " << channel << std::endl;
    }
}

void WsSpotClient::handle_user_data(const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    
    std::string event_type;
    if (data.contains("e")) {
        event_type = data["e"].get<std::string>();
    }
    
    if (event_type == "outboundAccountPosition") {
        if (account_update_callback_) {
            account_update_callback_(data);
        }
    } else if (event_type == "executionReport") {
        if (order_update_callback_) {
            order_update_callback_(data);
        }
    } else if (event_type == "balanceUpdate") {
        if (balance_update_callback_) {
            balance_update_callback_(data);
        }
    }
}

std::string WsSpotClient::build_subscribe_message(const Subscription& sub) {
    nlohmann::json msg;
    
    // MEXC v3 WebSocket API uses channel-based subscriptions
    // Format: spot@public.{stream}.v3.api@{symbol}
    std::string channel;
    
    if (sub.method == "sub.kline") {
        channel = "spot@public.kline.v3.api@" + sub.interval + "@" + sub.symbol;
    } else if (sub.method == "sub.depth") {
        // MEXC v3 aggregated depth channel with Protobuf format
        // Format: spot@public.aggre.depth.v3.api.pb@{interval}@{symbol}
        // Using 100ms update interval (can be 10ms, 100ms, etc.)
        // Note: This channel uses Protobuf binary format, requires deserialization
        channel = "spot@public.aggre.depth.v3.api.pb@100ms@" + sub.symbol;
    } else if (sub.method == "sub.ticker") {
        channel = "spot@public.ticker.v3.api@" + sub.symbol;
    } else if (sub.method == "sub.trades") {
        channel = "spot@public.deals.v3.api@" + sub.symbol;
    } else if (sub.method == "sub.bookTicker") {
        channel = "spot@public.aggre.bookTicker.v3.api.pb@100ms@" + sub.symbol;
    } else if (sub.method == "sub.miniTicker") {
        channel = "spot@public.miniTicker.v3.api@" + sub.symbol;
    } else {
        // Fallback for unknown methods
        channel = "spot@public." + sub.method + ".v3.api@" + sub.symbol;
    }
    
    // MEXC v3 format: {"method": "SUBSCRIPTION", "params": ["channel"], "id": unique_id}
    static int subscription_id = 1;
    msg["method"] = "SUBSCRIPTION";
    msg["params"] = nlohmann::json::array({channel});
    msg["id"] = subscription_id++;
    
    return msg.dump();
}

std::string WsSpotClient::build_unsubscribe_message(const Subscription& sub) {
    nlohmann::json msg;
    
    // MEXC uses UNSUBSCRIPTION method
    msg["method"] = "UNSUBSCRIPTION";
    
    // Build the channel name to unsubscribe from
    std::string channel;
    if (sub.method == "sub.kline") {
        channel = "spot@public.kline.v3.api@" + sub.interval + "@" + sub.symbol;
    } else if (sub.method == "sub.depth") {
        // Use aggregated depth format (JSON version)
        channel = "spot@public.aggre.depth.v3.api@100ms@" + sub.symbol;
    } else if (sub.method == "sub.ticker") {
        channel = "spot@public.ticker.v3.api@" + sub.symbol;
    } else if (sub.method == "sub.trades") {
        channel = "spot@public.deals.v3.api@" + sub.symbol;
    } else if (sub.method == "sub.bookTicker") {
        channel = "spot@public.aggre.bookTicker.v3.api.pb@100ms@" + sub.symbol;
    } else if (sub.method == "sub.miniTicker") {
        channel = "spot@public.miniTicker.v3.api@" + sub.symbol;
    } else {
        channel = "spot@public." + sub.method + ".v3.api@" + sub.symbol;
    }
    
    msg["params"] = nlohmann::json::array({channel});
    static int unsubscription_id = 10000; // Use different ID range for unsubscribes
    msg["id"] = unsubscription_id++;
    
    return msg.dump();
}

void WsSpotClient::resubscribe_all() {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    for (const auto& sub : active_subscriptions_) {
        std::string msg = build_subscribe_message(sub);
        public_ws_->send(msg);
    }
}

std::string WsSpotClient::build_user_data_listen_key_request() {
    // This would be called via REST API to get a listen key
    // Placeholder for now
    return "";
}

void WsSpotClient::authenticate() {
    // WebSocket authentication for user data streams
    // This typically involves sending an auth message with API key and signature
    // Placeholder for now
}

} // namespace mexc

