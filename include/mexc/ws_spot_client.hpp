#pragma once

#include "mexc/ws_client.hpp"
#include "mexc/client_base.hpp"
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace mexc {

class SpotClient; // Forward declaration

// Market data callbacks
using TickerCallback = std::function<void(const nlohmann::json&)>;
using DepthCallback = std::function<void(const nlohmann::json&)>;
using TradeCallback = std::function<void(const nlohmann::json&)>;
using KlineCallback = std::function<void(const nlohmann::json&)>;
using BookTickerCallback = std::function<void(const nlohmann::json&)>;

// User data callbacks
using AccountUpdateCallback = std::function<void(const nlohmann::json&)>;
using OrderUpdateCallback = std::function<void(const nlohmann::json&)>;
using BalanceUpdateCallback = std::function<void(const nlohmann::json&)>;

class WsSpotClient {
public:
    explicit WsSpotClient(Credentials credentials,
                         const std::string& base_ws_url = "wss://wbs-api.mexc.com/ws",
                         SpotClient* rest_client = nullptr);

    ~WsSpotClient();

    WsSpotClient(const WsSpotClient&) = delete;
    WsSpotClient& operator=(const WsSpotClient&) = delete;
    WsSpotClient(WsSpotClient&&) noexcept = delete;
    WsSpotClient& operator=(WsSpotClient&&) noexcept = delete;

    // Connection management
    bool connect();
    void disconnect();
    bool is_connected() const;

    // Market data streams (public)
    // Subscribe to ticker updates for a symbol
    bool subscribe_ticker(const std::string& symbol);
    bool unsubscribe_ticker(const std::string& symbol);

    // Subscribe to depth updates (order book)
    bool subscribe_depth(const std::string& symbol, int limit = 20);
    bool unsubscribe_depth(const std::string& symbol);

    // Subscribe to trade updates
    bool subscribe_trades(const std::string& symbol);
    bool unsubscribe_trades(const std::string& symbol);

    // Subscribe to kline/candlestick updates
    bool subscribe_klines(const std::string& symbol, const std::string& interval);
    bool unsubscribe_klines(const std::string& symbol, const std::string& interval);

    // Subscribe to book ticker (best bid/ask)
    bool subscribe_book_ticker(const std::string& symbol);
    bool unsubscribe_book_ticker(const std::string& symbol);

    // Subscribe to mini ticker (24hr stats)
    bool subscribe_mini_ticker(const std::string& symbol);
    bool unsubscribe_mini_ticker(const std::string& symbol);

    // User data streams (private, requires authentication)
    // Note: Requires a SpotClient instance to get listen key
    bool subscribe_user_data();
    void unsubscribe_user_data();
    bool refresh_listen_key(); // Extend listen key validity

    // Callbacks for market data
    void set_ticker_callback(TickerCallback callback);
    void set_depth_callback(DepthCallback callback);
    void set_trade_callback(TradeCallback callback);
    void set_kline_callback(KlineCallback callback);
    void set_book_ticker_callback(BookTickerCallback callback);

    // Callbacks for user data
    void set_account_update_callback(AccountUpdateCallback callback);
    void set_order_update_callback(OrderUpdateCallback callback);
    void set_balance_update_callback(BalanceUpdateCallback callback);

    // Error and state callbacks
    void set_error_callback(WsErrorCallback callback);
    void set_state_callback(WsStateCallback callback);

    // Configuration
    void set_auto_reconnect(bool enable, int max_reconnect_attempts = -1);
    void set_reconnect_delay_ms(int delay_ms);

private:
    struct Subscription {
        std::string method;
        std::string symbol;
        std::string interval; // for klines
        int limit = 20; // for depth
    };

    void handle_message(const std::string& message);
    void handle_binary_message(const std::vector<uint8_t>& data);
    void handle_market_data(const nlohmann::json& data);
    void handle_user_data(const nlohmann::json& data);
    
    std::string build_subscribe_message(const Subscription& sub);
    std::string build_unsubscribe_message(const Subscription& sub);
    std::string build_user_data_listen_key_request();
    
    void resubscribe_all();
    void authenticate();

    Credentials credentials_;
    std::string base_ws_url_;
    SpotClient* rest_client_; // Optional REST client for listen key management
    std::unique_ptr<WsClient> public_ws_;
    std::unique_ptr<WsClient> user_ws_;
    
    std::vector<Subscription> active_subscriptions_;
    std::mutex subscriptions_mutex_;
    
    std::string listen_key_;
    std::mutex listen_key_mutex_;
    
    // Callbacks
    TickerCallback ticker_callback_;
    DepthCallback depth_callback_;
    TradeCallback trade_callback_;
    KlineCallback kline_callback_;
    BookTickerCallback book_ticker_callback_;
    
    AccountUpdateCallback account_update_callback_;
    OrderUpdateCallback order_update_callback_;
    BalanceUpdateCallback balance_update_callback_;
    
    WsErrorCallback error_callback_;
    WsStateCallback state_callback_;
    
    std::mutex callbacks_mutex_;
    
    bool auto_reconnect_;
    int max_reconnect_attempts_;
    int reconnect_delay_ms_;
};

} // namespace mexc

