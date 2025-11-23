#pragma once

#include "mexc/spot_client.hpp"
#include "strategy/trade_ledger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace strategy {

struct MarketMakerConfig {
    std::string symbol = "SPYXUSDT";
    std::string ledger_path = "data/trade_ledger.jsonl";
    double quote_budget = 10.0;
    double min_quote_order = 1.0;
    double min_base_quantity = 0.0005;
    double spread_bps = 20.0;
    double min_edge_bps = 5.0;
    double inventory_target = 0.5;
    double inventory_tolerance = 0.10;
    double max_inventory_ratio = 0.8;
    double escape_bps = 25.0;
    double maker_fee = 0.0;
    double taker_fee = 0.0005;
    double quantity_increment = 0.0001;
    double quote_increment = 0.01;
    double max_drawdown_pct = 0.2;      // 20% drawdown stop
    double max_drawdown_usd = 10.0;     // or $10 absolute
    int price_precision = 4;
    int quantity_precision = 4;
    int quote_precision = 2;
    int refresh_interval_ms = 1000;
    int account_staleness_ms = 2000;
    int depth_staleness_ms = 1500;
    int order_status_poll_ms = 200;
    int order_status_timeout_ms = 2000;
    int risk_cooldown_ms = 60000;
    int taker_escape_cooldown_ms = 5000;
    int max_taker_escapes_per_min = 6;
    int rate_limit_backoff_ms_initial = 750;
    int rate_limit_backoff_ms_max = 10000;
    int fill_poll_interval_ms = 2000;
    double escape_hysteresis_bps = 5.0;
    int min_escape_interval_ms = 1500;
};

struct OrderBookSnapshot {
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread = 0.0;
    double bid_volume = 0.0;
    double ask_volume = 0.0;
    double microprice = 0.0;
};

struct WorkingOrder {
    std::string client_id;
    std::string side;
    double price = 0.0;
    double quantity = 0.0;
};

struct FillRecord {
    std::chrono::system_clock::time_point timestamp;
    std::string side;
    double price = 0.0;
    double quantity = 0.0;
    double notional = 0.0;
    bool is_taker = false;
};

class MarketMakerStrategy {
public:
    MarketMakerStrategy(mexc::SpotClient& client, MarketMakerConfig config);

    void run();

private:
    struct SymbolFilters {
        double min_price = 0.0;
        double tick_size = 0.0;
        double min_qty = 0.0;
        double step_size = 0.0;
        double min_notional = 0.0;
    };

    void refresh_balances(const nlohmann::json& account_info);
    OrderBookSnapshot parse_order_book(const nlohmann::json& depth_json);
    std::unordered_set<std::string> extract_open_client_order_ids(const nlohmann::json& open_orders);
    void reconcile_orders(const std::unordered_set<std::string>& open_ids);
    void enforce_escape_conditions(const OrderBookSnapshot& book,
                                   const std::unordered_set<std::string>& open_ids);
    bool ensure_starting_inventory(const OrderBookSnapshot& book);
    void maintain_quotes(const OrderBookSnapshot& book);
    void pull_recent_trades(const OrderBookSnapshot& book);
    void report_pnl(double nav, double base_share, bool first_iteration);
    bool enforce_risk_limits(double nav, double base_share);
    void cancel_all_quotes();
    double compute_nav(const OrderBookSnapshot& book) const;
    double compute_base_share(double nav, const OrderBookSnapshot& book) const;

    bool place_limit_order(const std::string& side,
                           double price,
                           double quantity,
                           const std::string& client_order_id);
    bool place_market_order(const std::string& side,
                            double quantity,
                            double quote_amount,
                            const std::string& reason_tag);

    void load_trade_ledger();
    void load_symbol_filters();
    void refresh_open_orders(const nlohmann::json& open_orders);
    bool wait_for_order_close(const std::string& client_id, const std::string& side);
    bool within_account_staleness(const std::chrono::system_clock::time_point& snapshot_time) const;
    void reset_risk_cooldown();
    bool throttle_taker_escape();
    bool validate_filters(double price, double quantity, double notional) const;
    static bool within_increment(double value, double increment);
    void note_rate_limit_hit();
    void note_request_success();

    static std::string make_order_id(const std::string& symbol, const std::string& side);
    static double floor_to_increment(double value, double increment);
    std::string format_decimal(double value, int precision) const;
    double round_down(double value, int precision) const;

    mexc::SpotClient& client_;
    MarketMakerConfig config_;
    std::string base_asset_;
    TradeLedger ledger_;
    int64_t base_scale_;
    int64_t quote_scale_;
    double base_balance_ = 0.0;
    double quote_balance_ = 0.0;
    double base_locked_ = 0.0;
    double quote_locked_ = 0.0;
    std::optional<WorkingOrder> buy_order_;
    std::optional<WorkingOrder> sell_order_;

    std::optional<double> initial_nav_;
    double initial_base_ = std::numeric_limits<double>::quiet_NaN();
    double initial_quote_ = std::numeric_limits<double>::quiet_NaN();
    std::optional<double> session_peak_nav_;
    bool trading_enabled_ = true;

    // Realized PnL tracking
    double position_base_ = 0.0;
    double position_cost_ = 0.0;
    double realized_pnl_ = 0.0;
    long long last_trade_id_ = 0;
    bool trade_cursor_initialized_ = false;
    bool position_initialized_ = false;
    std::vector<FillRecord> fills_;
    std::optional<SymbolFilters> symbol_filters_;
    std::chrono::system_clock::time_point last_account_update_;
    long long last_depth_update_id_ = 0;
    std::chrono::system_clock::time_point last_depth_fetch_time_;
    std::chrono::system_clock::time_point risk_disabled_since_;
    std::chrono::system_clock::time_point last_escape_time_;
    std::chrono::system_clock::time_point escape_window_start_;
    int escape_count_window_ = 0;
    std::chrono::steady_clock::time_point rate_limited_until_;
    double current_backoff_ms_ = 0.0;
    bool rate_limited_this_loop_ = false;
    std::chrono::steady_clock::time_point last_trades_poll_time_;
    std::chrono::system_clock::time_point last_sell_escape_event_;
    std::chrono::system_clock::time_point last_buy_escape_event_;
    double last_sell_escape_price_ = 0.0;
    double last_buy_escape_price_ = 0.0;
};

} // namespace strategy
