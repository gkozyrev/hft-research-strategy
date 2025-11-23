#include "strategy/market_maker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <numeric>
#include <thread>

namespace strategy {
namespace {

constexpr double kBasisPoint = 0.0001;
constexpr int kDepthLevels = 5;
constexpr double kEpsilon = 1e-9;
constexpr double kPriceCompareEps = 1e-6;

double parse_double_optional(const nlohmann::json& value);
long long parse_id_optional(const nlohmann::json& value);
std::string parse_string_optional(const nlohmann::json& value);
std::string get_string_optional(const nlohmann::json& obj, const char* key);
bool get_bool_optional(const nlohmann::json& obj, const char* key, bool default_value);
long long get_id_optional(const nlohmann::json& obj, const char* key, long long default_value);

struct BalanceDetail {
    double free = 0.0;
    double locked = 0.0;
};

std::optional<BalanceDetail> extract_balance(const nlohmann::json& balances, const std::string& asset) {
    const auto it = std::find_if(balances.begin(), balances.end(), [&](const nlohmann::json& entry) {
        return entry.contains("asset") && entry["asset"].get<std::string>() == asset;
    });

    if (it == balances.end()) {
        return std::nullopt;
    }

    try {
        BalanceDetail detail;
        if ((*it).contains("free")) {
            detail.free = parse_double_optional((*it)["free"]);
        }
        if ((*it).contains("locked")) {
            detail.locked = parse_double_optional((*it)["locked"]);
        }
        return detail;
    } catch (...) {
        return std::nullopt;
    }
}

std::string base_asset_from_symbol(const std::string& symbol) {
    const auto pos = symbol.find("USDT");
    if (pos == std::string::npos) {
        return symbol;
    }
    return symbol.substr(0, pos);
}

double parse_double_optional(const nlohmann::json& value) {
    if (value.is_null()) {
        return 0.0;
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    if (value.is_number_float() || value.is_number_integer()) {
        return value.get<double>();
    }
    return 0.0;
}

long long parse_id_optional(const nlohmann::json& value) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    if (value.is_number_float()) {
        return static_cast<long long>(value.get<double>());
    }
    return 0;
}

std::string parse_string_optional(const nlohmann::json& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return {};
}

std::string get_string_optional(const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key)) {
        return {};
    }
    return parse_string_optional(obj.at(key));
}

bool get_bool_optional(const nlohmann::json& obj, const char* key, bool default_value = false) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return default_value;
    }
    const auto& value = obj.at(key);
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return text == "true" || text == "1";
    }
    return default_value;
}

long long get_id_optional(const nlohmann::json& obj, const char* key, long long default_value = 0) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return default_value;
    }
    return parse_id_optional(obj.at(key));
}

int64_t pow10_int(int precision) {
    if (precision <= 0) {
        return 1;
    }
    int64_t value = 1;
    for (int i = 0; i < precision; ++i) {
        value *= 10;
    }
    return value;
}

std::atomic<uint64_t> g_order_counter{0};

int precision_from_step(double step) {
    if (step <= 0.0) {
        return 0;
    }
    int precision = 0;
    double value = step;
    while (precision < 12 && std::fabs(value - std::round(value)) > 1e-9) {
        value *= 10.0;
        ++precision;
    }
    return std::clamp(precision, 0, 8);
}

} // namespace

MarketMakerStrategy::MarketMakerStrategy(mexc::SpotClient& client, MarketMakerConfig config)
    : client_(client),
      config_(std::move(config)),
      base_asset_(base_asset_from_symbol(config_.symbol)),
      ledger_(TradeLedgerConfig{
          config_.ledger_path,
          static_cast<int>(pow10_int(config_.quantity_precision)),
          static_cast<int>(pow10_int(config_.quote_precision))
      }),
      base_scale_(pow10_int(config_.quantity_precision)),
      quote_scale_(pow10_int(config_.quote_precision)) {
    load_trade_ledger();
    load_symbol_filters();
}

std::string MarketMakerStrategy::make_order_id(const std::string& symbol, const std::string& side) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream oss;
    if (!symbol.empty()) {
        oss << symbol.front();
    }
    const char tag = side.empty() ? 'X' : static_cast<char>(std::toupper(side.front()));
    const auto seq = g_order_counter.fetch_add(1, std::memory_order_relaxed) % 10000;
    oss << tag << ms << std::setw(4) << std::setfill('0') << seq;
    std::string id = oss.str();
    if (id.size() > 32) {
        id.resize(32);
    }
    return id;
}

double MarketMakerStrategy::floor_to_increment(double value, double increment) {
    if (increment <= kEpsilon || value <= 0.0) {
        return std::max(0.0, value);
    }
    const double scaled = std::floor(value / increment);
    return scaled * increment;
}

std::string MarketMakerStrategy::format_decimal(double value, int precision) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

double MarketMakerStrategy::round_down(double value, int precision) const {
    if (precision < 0) {
        return value;
    }
    const double factor = std::pow(10.0, precision);
    return std::floor(value * factor) / factor;
}

void MarketMakerStrategy::load_trade_ledger() {
    try {
        const auto state = ledger_.load();
        position_base_ = static_cast<double>(state.position_base) / static_cast<double>(base_scale_);
        position_cost_ = static_cast<double>(state.position_cost) / static_cast<double>(quote_scale_);
        realized_pnl_ = static_cast<double>(state.realized_pnl) / static_cast<double>(quote_scale_);
        last_trade_id_ = state.last_trade_id;
        trade_cursor_initialized_ = (last_trade_id_ > 0);
        position_initialized_ = (state.position_base > 0 || state.position_cost > 0);

        if (trade_cursor_initialized_) {
            std::cout << "[Ledger] Restored last trade id " << last_trade_id_
                      << " position=" << position_base_
                      << " cost=" << position_cost_
                      << " realized=" << realized_pnl_ << std::endl;
        } else {
            std::cout << "[Ledger] No prior fills found; starting fresh." << std::endl;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Ledger] Failed to load ledger: " << ex.what() << std::endl;
    }
}

void MarketMakerStrategy::load_symbol_filters() {
    try {
        const auto response = client_.exchange_info(config_.symbol);
        const auto json = nlohmann::json::parse(response);
        if (!json.contains("symbols")) {
            return;
        }

        const auto& symbols = json["symbols"];
        const auto it = std::find_if(symbols.begin(), symbols.end(), [&](const nlohmann::json& entry) {
            return entry.value("symbol", std::string{}) == config_.symbol;
        });
        if (it == symbols.end() || !(*it).contains("filters")) {
            return;
        }

        SymbolFilters filters;
        for (const auto& filter : (*it)["filters"]) {
            const auto type = get_string_optional(filter, "filterType");
            if (type == "PRICE_FILTER") {
                filters.min_price = parse_double_optional(filter["minPrice"]);
                filters.tick_size = parse_double_optional(filter["tickSize"]);
            } else if (type == "LOT_SIZE") {
                filters.min_qty = parse_double_optional(filter["minQty"]);
                filters.step_size = parse_double_optional(filter["stepSize"]);
            } else if (type == "MIN_NOTIONAL") {
                filters.min_notional = parse_double_optional(filter["minNotional"]);
            }
        }

        symbol_filters_ = filters;

        if (filters.step_size > 0.0 && std::fabs(filters.step_size - config_.quantity_increment) > 1e-8) {
            std::cout << "[Config] Adjusting quantity increment from " << config_.quantity_increment
                      << " to exchange step size " << filters.step_size << std::endl;
            config_.quantity_increment = filters.step_size;
        }
        if (filters.step_size > 0.0) {
            config_.quantity_precision = std::max(config_.quantity_precision, precision_from_step(filters.step_size));
        }
        if (filters.tick_size > 0.0 && std::fabs(filters.tick_size - std::pow(10.0, -config_.price_precision)) > 1e-8) {
            std::cout << "[Config] Exchange tick size " << filters.tick_size
                      << " differs from configured precision; ensure rounding aligns." << std::endl;
        }
        if (filters.tick_size > 0.0) {
            config_.price_precision = std::max(config_.price_precision, precision_from_step(filters.tick_size));
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Config] Failed to load symbol filters: " << ex.what() << std::endl;
    }
}

void MarketMakerStrategy::run() {
    std::cout << "[Strategy] Starting market making on " << config_.symbol << std::endl;
    bool first_iteration = true;
    const auto refresh_period = std::chrono::milliseconds(config_.refresh_interval_ms);

    while (true) {
        const auto wait_now = std::chrono::steady_clock::now();
        if (rate_limited_until_.time_since_epoch().count() != 0 && wait_now < rate_limited_until_) {
            std::this_thread::sleep_for(rate_limited_until_ - wait_now);
            continue;
        }

        rate_limited_this_loop_ = false;
        const auto loop_start = std::chrono::steady_clock::now();
        try {
            const auto account_json = nlohmann::json::parse(client_.account_info());
            refresh_balances(account_json);

            const auto open_orders_json = nlohmann::json::parse(client_.open_orders(config_.symbol));
            refresh_open_orders(open_orders_json);
            const auto open_ids = extract_open_client_order_ids(open_orders_json);
            reconcile_orders(open_ids);

            const auto depth_json = nlohmann::json::parse(client_.depth(config_.symbol, kDepthLevels));
            const auto book = parse_order_book(depth_json);

            if (!position_initialized_) {
                const double mark = (book.microprice > kEpsilon) ? book.microprice : std::max(book.best_bid, book.best_ask);
                position_base_ = base_balance_ + base_locked_;
                position_cost_ = position_base_ * mark;
                position_initialized_ = true;
            }

            pull_recent_trades(book);
            enforce_escape_conditions(book, open_ids);

            const double nav = compute_nav(book);
            const double base_share = compute_base_share(nav, book);
            const bool risk_ok = enforce_risk_limits(nav, base_share);
            report_pnl(nav, base_share, first_iteration);
            first_iteration = false;

            if (!risk_ok) {
                note_request_success();
                std::this_thread::sleep_for(refresh_period);
                continue;
            }

            if (!ensure_starting_inventory(book)) {
                note_request_success();
                std::this_thread::sleep_for(refresh_period);
                continue;
            }

            maintain_quotes(book);

            note_request_success();
        } catch (const mexc::HttpError& ex) {
            std::cerr << "[Strategy] HTTP error: " << ex.what() << " (status " << ex.status_code() << ")" << std::endl;
            if (ex.status_code() == 429) {
                note_rate_limit_hit();
            }
            std::this_thread::sleep_for(refresh_period);
        } catch (const std::exception& ex) {
            std::cerr << "[Strategy] Unexpected error: " << ex.what() << std::endl;
        }

        if (rate_limited_this_loop_) {
            // already backed off in note_rate_limit_hit()
        }

        const auto elapsed = std::chrono::steady_clock::now() - loop_start;
        if (elapsed < refresh_period) {
            std::this_thread::sleep_for(refresh_period - elapsed);
        }
    }
}

void MarketMakerStrategy::refresh_balances(const nlohmann::json& json) {
    if (!json.contains("balances")) {
        throw std::runtime_error("Account info response missing balances");
    }

    const auto& balances = json["balances"];
    const auto quote = extract_balance(balances, "USDT").value_or(BalanceDetail{});
    const auto base = extract_balance(balances, base_asset_).value_or(BalanceDetail{});

    quote_balance_ = std::max(0.0, quote.free);
    quote_locked_ = std::max(0.0, quote.locked);
    base_balance_ = std::max(0.0, base.free);
    base_locked_ = std::max(0.0, base.locked);

    const auto update_ms = get_id_optional(json, "updateTime", 0);
    if (update_ms > 0) {
        last_account_update_ = std::chrono::system_clock::time_point{std::chrono::milliseconds(update_ms)};
    } else {
        last_account_update_ = std::chrono::system_clock::now();
    }

    if (!within_account_staleness(last_account_update_)) {
        throw std::runtime_error("Account snapshot stale; aborting iteration");
    }

    std::cout << "[Strategy] Balances -> " << base_asset_ << ": free=" << base_balance_
              << " locked=" << base_locked_
              << ", USDT free=" << quote_balance_
              << " locked=" << quote_locked_ << std::endl;
}

OrderBookSnapshot MarketMakerStrategy::parse_order_book(const nlohmann::json& json) {
    OrderBookSnapshot book;

    auto accumulate_filtered = [&](const nlohmann::json& side, const std::optional<WorkingOrder>& own_order) {
        double best_price = 0.0;
        double volume = 0.0;
        int counted = 0;
        for (const auto& level : side) {
            double price = 0.0;
            double qty = 0.0;
            try {
                price = parse_double_optional(level[0]);
                qty = parse_double_optional(level[1]);
            } catch (...) {
                continue;
            }

            if (own_order && std::fabs(price - own_order->price) <= kPriceCompareEps) {
                continue;
            }

            if (best_price <= 0.0) {
                best_price = price;
            }

            if (counted < kDepthLevels) {
                volume += price * qty;
                counted++;
            }
        }
        return std::make_pair(best_price, volume);
    };

    std::pair<double, double> bid_info{0.0, 0.0};
    std::pair<double, double> ask_info{0.0, 0.0};

    if (json.contains("bids") && json["bids"].is_array()) {
        bid_info = accumulate_filtered(json["bids"], buy_order_);
    }

    if (json.contains("asks") && json["asks"].is_array()) {
        ask_info = accumulate_filtered(json["asks"], sell_order_);
    }

    book.best_bid = bid_info.first;
    book.best_ask = ask_info.first;
    book.bid_volume = bid_info.second;
    book.ask_volume = ask_info.second;
    if (book.best_bid > 0.0 && book.best_ask > 0.0) {
        book.spread = book.best_ask - book.best_bid;
    }

    double bid_qty = 0.0;
    double ask_qty = 0.0;
    if (book.best_bid > 0.0 && json.contains("bids")) {
        for (const auto& level : json["bids"]) {
            try {
                const double price = parse_double_optional(level[0]);
                if (std::fabs(price - book.best_bid) <= kPriceCompareEps) {
                    bid_qty = parse_double_optional(level[1]);
                    if (buy_order_ && std::fabs(price - buy_order_->price) <= kPriceCompareEps) {
                        bid_qty = std::max(0.0, bid_qty - buy_order_->quantity);
                    }
                    break;
                }
            } catch (...) {
                continue;
            }
        }
    }

    if (book.best_ask > 0.0 && json.contains("asks")) {
        for (const auto& level : json["asks"]) {
            try {
                const double price = parse_double_optional(level[0]);
                if (std::fabs(price - book.best_ask) <= kPriceCompareEps) {
                    ask_qty = parse_double_optional(level[1]);
                    if (sell_order_ && std::fabs(price - sell_order_->price) <= kPriceCompareEps) {
                        ask_qty = std::max(0.0, ask_qty - sell_order_->quantity);
                    }
                    break;
                }
            } catch (...) {
                continue;
            }
        }
    }

    if (book.best_bid > 0.0 && book.best_ask > 0.0) {
        const double denom = bid_qty + ask_qty;
        if (denom > kEpsilon) {
            book.microprice = (book.best_bid * ask_qty + book.best_ask * bid_qty) / denom;
        } else {
            book.microprice = (book.best_bid + book.best_ask) / 2.0;
        }
    } else {
        book.microprice = std::max(book.best_bid, book.best_ask);
    }

    if (json.contains("lastUpdateId")) {
        const long long update_id = json["lastUpdateId"].get<long long>();
        if (update_id < last_depth_update_id_) {
            throw std::runtime_error("Received out-of-order depth snapshot");
        }
        last_depth_update_id_ = update_id;
    }
    last_depth_fetch_time_ = std::chrono::system_clock::now();

    std::cout << "[Strategy] Market(ex-self) -> best bid: " << book.best_bid
              << ", best ask: " << book.best_ask
              << ", spread: " << book.spread << std::endl;

    return book;
}

std::unordered_set<std::string> MarketMakerStrategy::extract_open_client_order_ids(const nlohmann::json& open_orders) {
    std::unordered_set<std::string> ids;
    if (!open_orders.is_array()) {
        return ids;
    }
    for (const auto& entry : open_orders) {
        const auto id = get_string_optional(entry, "clientOrderId");
        if (!id.empty()) {
            ids.insert(id);
        }
    }
    return ids;
}

void MarketMakerStrategy::refresh_open_orders(const nlohmann::json& open_orders) {
    buy_order_.reset();
    sell_order_.reset();

    if (!open_orders.is_array()) {
        return;
    }

    for (const auto& entry : open_orders) {
        const auto client_id = get_string_optional(entry, "clientOrderId");
        const auto side = get_string_optional(entry, "side");
        const double price = entry.contains("price") ? parse_double_optional(entry["price"]) : 0.0;
        const double orig_qty = entry.contains("origQty") ? parse_double_optional(entry["origQty"]) : 0.0;
        const double executed = entry.contains("executedQty") ? parse_double_optional(entry["executedQty"]) : 0.0;
        const double remaining = std::max(0.0, orig_qty - executed);

        if (client_id.empty() || side.empty() || price <= 0.0 || remaining < config_.min_base_quantity) {
            continue;
        }

        WorkingOrder order{client_id, side, price, remaining};
        if (side == "BUY") {
            if (!buy_order_ || price > buy_order_->price) {
                buy_order_ = order;
            }
        } else if (side == "SELL") {
            if (!sell_order_ || price < sell_order_->price) {
                sell_order_ = order;
            }
        }
    }
}

bool MarketMakerStrategy::wait_for_order_close(const std::string& client_id, const std::string& side) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config_.order_status_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.order_status_poll_ms));
        try {
            mexc::QueryParams params = {
                {"origClientOrderId", client_id},
                {"recvWindow", "10000"}
            };
            const auto response = client_.query_order(config_.symbol, std::move(params));
            const auto json = nlohmann::json::parse(response);
            const auto status = get_string_optional(json, "status");
            if (status == "CANCELED" || status == "FILLED" || status == "REJECTED" || status == "EXPIRED") {
                std::cout << "[Strategy] Confirmed " << side << " order " << client_id
                          << " closed with status " << status << std::endl;
                return true;
            }
        } catch (const mexc::HttpError& ex) {
            std::cerr << "[Strategy] Failed to query order status for " << client_id << ": "
                      << ex.what() << std::endl;
            if (ex.status_code() == 429) {
                note_rate_limit_hit();
            }
        } catch (const std::exception& ex) {
            std::cerr << "[Strategy] Failed to query order status for " << client_id << ": "
                      << ex.what() << std::endl;
        }
    }

    std::cerr << "[Strategy] Timed out waiting for " << side << " order " << client_id << " to close" << std::endl;
    return false;
}

void MarketMakerStrategy::reconcile_orders(const std::unordered_set<std::string>& open_ids) {
    if (sell_order_ && !open_ids.count(sell_order_->client_id)) {
        std::cout << "[Strategy] Sell order closed: " << sell_order_->client_id << std::endl;
        sell_order_.reset();
    }
    if (buy_order_ && !open_ids.count(buy_order_->client_id)) {
        std::cout << "[Strategy] Buy order closed: " << buy_order_->client_id << std::endl;
        buy_order_.reset();
    }
}

void MarketMakerStrategy::enforce_escape_conditions(const OrderBookSnapshot& book,
                                                    const std::unordered_set<std::string>& open_ids) {
    const double escape_fraction = config_.escape_bps * kBasisPoint;
    const double hysteresis_fraction = config_.escape_hysteresis_bps * kBasisPoint;
    const auto now = std::chrono::system_clock::now();
    const auto min_interval = std::chrono::milliseconds(config_.min_escape_interval_ms);

    if (sell_order_ && open_ids.count(sell_order_->client_id)) {
        if (last_sell_escape_event_.time_since_epoch().count() == 0 ||
            now - last_sell_escape_event_ >= min_interval) {
            const double threshold = sell_order_->price * (1.0 - escape_fraction);
            double adjusted_threshold = threshold - sell_order_->price * hysteresis_fraction;
            if (last_sell_escape_price_ > 0.0) {
                adjusted_threshold = std::min(adjusted_threshold,
                                              last_sell_escape_price_ - sell_order_->price * hysteresis_fraction);
            }
            if (book.best_bid > kEpsilon && book.best_bid < adjusted_threshold) {
                std::cout << "[Strategy] Sell escape triggered at bid " << book.best_bid
                          << " (threshold " << threshold << ")" << std::endl;
                mexc::QueryParams params = {
                    {"origClientOrderId", sell_order_->client_id},
                    {"recvWindow", "10000"}
                };
                try {
                    client_.cancel_order(config_.symbol, std::move(params));
                } catch (const std::exception& ex) {
                    std::cerr << "[Strategy] Failed to cancel sell order: " << ex.what() << std::endl;
                    if (const auto* http_ex = dynamic_cast<const mexc::HttpError*>(&ex); http_ex && http_ex->status_code() == 429) {
                        note_rate_limit_hit();
                    }
                }

                wait_for_order_close(sell_order_->client_id, "SELL");

                const double notional = sell_order_->quantity * std::max(book.best_bid, kEpsilon);
                const double min_notional = symbol_filters_ ? symbol_filters_->min_notional : config_.min_quote_order;
                if (notional >= std::max(config_.min_quote_order, min_notional) && throttle_taker_escape()) {
                    place_market_order("SELL", sell_order_->quantity, notional, "escape_sell");
                }
                sell_order_.reset();
                last_sell_escape_event_ = now;
                last_sell_escape_price_ = book.best_bid;
            }
        }
    }

    if (buy_order_ && open_ids.count(buy_order_->client_id)) {
        if (last_buy_escape_event_.time_since_epoch().count() == 0 ||
            now - last_buy_escape_event_ >= min_interval) {
            const double threshold = buy_order_->price * (1.0 + escape_fraction);
            double adjusted_threshold = threshold + buy_order_->price * hysteresis_fraction;
            if (last_buy_escape_price_ > 0.0) {
                adjusted_threshold = std::max(adjusted_threshold,
                                              last_buy_escape_price_ + buy_order_->price * hysteresis_fraction);
            }
            if (book.best_ask > adjusted_threshold) {
                std::cout << "[Strategy] Buy escape triggered at ask " << book.best_ask
                          << " (threshold " << threshold << ")" << std::endl;
                mexc::QueryParams params = {
                    {"origClientOrderId", buy_order_->client_id},
                    {"recvWindow", "10000"}
                };
                try {
                    client_.cancel_order(config_.symbol, std::move(params));
                } catch (const std::exception& ex) {
                    std::cerr << "[Strategy] Failed to cancel buy order: " << ex.what() << std::endl;
                    if (const auto* http_ex = dynamic_cast<const mexc::HttpError*>(&ex); http_ex && http_ex->status_code() == 429) {
                        note_rate_limit_hit();
                    }
                }

                wait_for_order_close(buy_order_->client_id, "BUY");

                const double notional = buy_order_->quantity * std::max(book.best_ask, kEpsilon);
                const double spend = std::min(quote_balance_, std::max(config_.min_quote_order, notional));
                const double min_notional = symbol_filters_ ? symbol_filters_->min_notional : config_.min_quote_order;
                if (spend >= std::max(config_.min_quote_order, min_notional) && throttle_taker_escape()) {
                    place_market_order("BUY", buy_order_->quantity, spend, "escape_buy");
                }
                buy_order_.reset();
                last_buy_escape_event_ = now;
                last_buy_escape_price_ = book.best_ask;
            }
        }
    }
}

bool MarketMakerStrategy::ensure_starting_inventory(const OrderBookSnapshot& book) {
    if (!trading_enabled_ || book.microprice <= kEpsilon) {
        return false;
    }

    bool ready = true;

    if (quote_balance_ + kEpsilon < config_.min_quote_order && base_balance_ > config_.min_base_quantity) {
        const double price = book.best_bid > kEpsilon ? book.best_bid : book.microprice;
        double desired_quote = std::max(config_.min_quote_order * 1.5, config_.quote_budget);
        double needed_quote = desired_quote - quote_balance_;
        needed_quote = std::max(config_.min_quote_order, needed_quote);

        double max_sell = std::max(0.0, base_balance_ - config_.min_base_quantity);
        double sell_qty = floor_to_increment(needed_quote / std::max(price, kEpsilon), config_.quantity_increment);
        sell_qty = std::min(sell_qty, max_sell);

        if (sell_qty >= config_.min_base_quantity) {
            const auto order_id = make_order_id(config_.symbol, "BOOT_SELL");
            if (place_limit_order("SELL", price, sell_qty, order_id)) {
                sell_order_ = WorkingOrder{order_id, "SELL", price, sell_qty};
                ready = false;
            }
        } else {
            ready = false;
        }
    }

    if (ready && base_balance_ + kEpsilon < config_.min_base_quantity && quote_balance_ >= config_.min_quote_order) {
        const double price = book.best_ask > kEpsilon ? book.best_ask : book.microprice;
        double buy_notional = std::min(quote_balance_, std::max(config_.min_quote_order, config_.quote_budget));
        buy_notional = floor_to_increment(buy_notional, config_.quote_increment);

        if (buy_notional >= config_.min_quote_order) {
            double buy_qty = floor_to_increment(buy_notional / std::max(price, kEpsilon), config_.quantity_increment);
            if (buy_qty >= config_.min_base_quantity) {
                const auto order_id = make_order_id(config_.symbol, "BOOT_BUY");
                if (place_limit_order("BUY", price, buy_qty, order_id)) {
                    buy_order_ = WorkingOrder{order_id, "BUY", price, buy_qty};
                    ready = false;
                }
            }
        } else {
            ready = false;
        }
    }

    return ready;
}

void MarketMakerStrategy::maintain_quotes(const OrderBookSnapshot& book) {
    if (!trading_enabled_) {
        std::cout << "[Strategy] Trading disabled by risk manager; skipping quotes." << std::endl;
        return;
    }

    if (book.microprice <= kEpsilon) {
        std::cerr << "[Strategy] Invalid microprice; skipping." << std::endl;
        return;
    }

    const double spread_fraction = (book.spread > 0.0 && book.microprice > 0.0)
        ? book.spread / book.microprice
        : 0.0;
    const double min_edge_fraction = std::max(config_.min_edge_bps * kBasisPoint,
                                              2.0 * config_.maker_fee + 0.0002);
    if (spread_fraction < min_edge_fraction) {
        std::cout << "[Strategy] Spread too tight (" << spread_fraction * 1e4
                  << " bps); skipping quoting." << std::endl;
        return;
    }

    const double total_base_inventory = base_balance_ + base_locked_;
    const double total_quote_inventory = quote_balance_ + quote_locked_;
    const double total_value = total_quote_inventory + total_base_inventory * book.microprice;
    if (total_value <= 0.0) {
        std::cerr << "[Strategy] No inventory to deploy." << std::endl;
        return;
    }

    const double target_base_value = total_value * config_.inventory_target;
    const double target_qty = target_base_value / std::max(book.microprice, kEpsilon);
    const double upper_qty = target_qty * (1.0 + config_.inventory_tolerance);
    const double lower_qty = target_qty * (1.0 - config_.inventory_tolerance);

    const double book_spread_fraction = spread_fraction;
    const double target_spread_fraction = std::clamp(
        std::max(config_.spread_bps * kBasisPoint, book_spread_fraction * 0.5),
        0.0005, 0.02);

    const double book_imbalance = (book.bid_volume + book.ask_volume) > 0.0
        ? (book.bid_volume - book.ask_volume) / (book.bid_volume + book.ask_volume)
        : 0.0;

    const double inventory_ratio = (total_value > 0.0)
        ? (total_base_inventory * book.microprice) / total_value
        : 0.0;

    const double inventory_deviation = (inventory_ratio - config_.inventory_target) / config_.inventory_tolerance;
    const double skew_bias = std::clamp(0.5 * book_imbalance - inventory_deviation, -1.0, 1.0);

    double buy_price = book.microprice * (1.0 - target_spread_fraction / 2.0 - 0.25 * skew_bias * target_spread_fraction);
    double sell_price = book.microprice * (1.0 + target_spread_fraction / 2.0 + 0.25 * skew_bias * target_spread_fraction);

    buy_price = round_down(buy_price, config_.price_precision);
    sell_price = round_down(sell_price, config_.price_precision);

    if (buy_price <= 0.0 || sell_price <= 0.0 || buy_price >= sell_price) {
        std::cerr << "[Strategy] Price rounding collapsed spread, skipping." << std::endl;
        return;
    }

    const double upper_guard = config_.max_inventory_ratio;
    const double lower_guard = 1.0 - config_.max_inventory_ratio;
    const double hysteresis = config_.inventory_tolerance * 0.5;

    const bool allow_sell = inventory_ratio > (lower_guard + hysteresis);
    const bool allow_buy = inventory_ratio < (upper_guard - hysteresis);

    const double free_base = base_balance_;
    if (!sell_order_ && allow_sell && total_base_inventory > lower_qty && free_base > config_.min_base_quantity) {
        double excess_base = std::max(0.0, total_base_inventory - lower_qty);
        double sell_capacity = std::max(0.0, free_base - config_.min_base_quantity);
        double sell_quantity = std::min({excess_base, sell_capacity, config_.quote_budget / std::max(sell_price, kEpsilon)});
        sell_quantity = floor_to_increment(sell_quantity, config_.quantity_increment);

        if (sell_quantity >= config_.min_base_quantity) {
            const auto order_id = make_order_id(config_.symbol, "SELL");
            if (place_limit_order("SELL", sell_price, sell_quantity, order_id)) {
                sell_order_ = WorkingOrder{order_id, "SELL", sell_price, sell_quantity};
            }
        }
    } else if (!sell_order_ && !allow_sell) {
        std::cout << "[Inventory] Sell side paused; base share below guard." << std::endl;
    }

    if (!buy_order_ && allow_buy && total_base_inventory < upper_qty && quote_balance_ >= config_.min_quote_order) {
        double buy_notional = std::min(config_.quote_budget, quote_balance_);
        buy_notional = std::max(buy_notional, config_.min_quote_order);
        buy_notional = floor_to_increment(buy_notional, config_.quote_increment);
        buy_notional = std::min(buy_notional, quote_balance_);

        if (buy_notional >= config_.min_quote_order) {
            double buy_quantity = floor_to_increment(buy_notional / std::max(buy_price, kEpsilon), config_.quantity_increment);
            if (buy_quantity >= config_.min_base_quantity) {
                const auto order_id = make_order_id(config_.symbol, "BUY");
                if (place_limit_order("BUY", buy_price, buy_quantity, order_id)) {
                    buy_order_ = WorkingOrder{order_id, "BUY", buy_price, buy_quantity};
                }
            }
        }
    } else if (!buy_order_ && !allow_buy) {
        std::cout << "[Inventory] Buy side paused; base share above guard." << std::endl;
    }
}

void MarketMakerStrategy::pull_recent_trades(const OrderBookSnapshot& book) {
    (void)book;
    if (config_.symbol.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_trades_poll_time_.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_trades_poll_time_).count();
        if (elapsed < config_.fill_poll_interval_ms) {
            return;
        }
    }
    last_trades_poll_time_ = now;

    try {
        const long long cursor = std::max(last_trade_id_, ledger_.state().last_trade_id);
        mexc::QueryParams params = {{"limit", "100"}};
        if (cursor > 0) {
            params.emplace_back("fromId", std::to_string(cursor + 1));
        }

        const auto response = client_.account_trade_list(config_.symbol, std::move(params));
        const auto trades = nlohmann::json::parse(response);
        note_request_success();
        if (!trades.is_array()) {
            return;
        }

        std::vector<nlohmann::json> new_trades;
        long long max_id = cursor;

        for (const auto& trade : trades) {
            const long long id = trade.contains("id") ? parse_id_optional(trade["id"]) : 0;
            if (id <= cursor) {
                continue;
            }
            new_trades.push_back(trade);
            max_id = std::max(max_id, id);
        }

        if (new_trades.empty()) {
            last_trade_id_ = max_id;
            trade_cursor_initialized_ = (max_id > 0);
            return;
        }

        std::sort(new_trades.begin(), new_trades.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            const long long id_a = a.contains("id") ? parse_id_optional(a["id"]) : 0;
            const long long id_b = b.contains("id") ? parse_id_optional(b["id"]) : 0;
            return id_a < id_b;
        });

        for (const auto& trade : new_trades) {
            const long long id = trade.contains("id") ? parse_id_optional(trade["id"]) : 0;
            const bool is_buyer = get_bool_optional(trade, "isBuyer", false);
            const bool is_maker = get_bool_optional(trade, "isMaker", false);
            const double price = trade.contains("price") ? parse_double_optional(trade["price"]) : 0.0;
            const double qty = trade.contains("qty") ? parse_double_optional(trade["qty"]) : 0.0;
            double quote_qty = trade.contains("quoteQty") ? parse_double_optional(trade["quoteQty"]) : price * qty;
            const double commission = trade.contains("commission") ? parse_double_optional(trade["commission"]) : 0.0;
            const std::string commission_asset = get_string_optional(trade, "commissionAsset");
            const auto fill_time_ms = get_id_optional(trade, "time", get_id_optional(trade, "tradeTime", 0));

            double effective_qty = qty;
            double effective_quote = quote_qty;

            int64_t fee_units = 0;
            if (!commission_asset.empty()) {
                if (commission_asset == base_asset_) {
                    effective_qty = std::max(0.0, effective_qty - commission);
                    fee_units = static_cast<int64_t>(std::llround(commission * base_scale_));
                } else if (commission_asset == "USDT") {
                    effective_quote = std::max(0.0, effective_quote - commission);
                    fee_units = static_cast<int64_t>(std::llround(commission * quote_scale_));
                }
            }

            const int64_t base_units = static_cast<int64_t>(std::llround(effective_qty * base_scale_));
            const int64_t quote_units = static_cast<int64_t>(std::llround(effective_quote * quote_scale_));

            const auto before_state = ledger_.state();

            TradeFill fill{
                id,
                std::chrono::system_clock::time_point{std::chrono::milliseconds(fill_time_ms)},
                is_buyer ? TradeSide::Buy : TradeSide::Sell,
                base_units,
                quote_units,
                fee_units,
                commission_asset,
                !is_maker
            };

            ledger_.append(fill);
            const auto after_state = ledger_.state();

            FillRecord record{
                fill.timestamp,
                is_buyer ? "BUY" : "SELL",
                price,
                static_cast<double>(base_units) / static_cast<double>(base_scale_),
                static_cast<double>(quote_units) / static_cast<double>(quote_scale_),
                !is_maker
            };
            fills_.push_back(record);

            const double realized_delta = static_cast<double>(after_state.realized_pnl - before_state.realized_pnl) /
                                          static_cast<double>(quote_scale_);

            std::cout << "[FILL] " << record.side << (is_maker ? " maker" : " taker")
                      << " qty=" << record.quantity
                      << " price=" << record.price
                      << " notional=" << record.notional;
            if (std::fabs(realized_delta) > 1e-6) {
                std::cout << " realized=" << realized_delta;
            }
            std::cout << std::endl;
        }

        const auto state = ledger_.state();
        position_base_ = static_cast<double>(state.position_base) / static_cast<double>(base_scale_);
        position_cost_ = static_cast<double>(state.position_cost) / static_cast<double>(quote_scale_);
        realized_pnl_ = static_cast<double>(state.realized_pnl) / static_cast<double>(quote_scale_);
        last_trade_id_ = state.last_trade_id;
        trade_cursor_initialized_ = (last_trade_id_ > 0);
        position_initialized_ = true;
    } catch (const mexc::HttpError& ex) {
        std::cerr << "[FILL] Failed to pull trades: " << ex.what() << std::endl;
        if (ex.status_code() == 429) {
            note_rate_limit_hit();
        }
    } catch (const std::exception& ex) {
        std::cerr << "[FILL] Failed to pull trades: " << ex.what() << std::endl;
    }
}

bool MarketMakerStrategy::place_limit_order(const std::string& side,
                                            double price,
                                            double quantity,
                                            const std::string& client_order_id) {
    if (price <= 0.0 || quantity <= 0.0) {
        return false;
    }

    quantity = floor_to_increment(quantity, config_.quantity_increment);
    const double notional = quantity * price;
    if (quantity < config_.min_base_quantity || notional < config_.min_quote_order) {
        return false;
    }

    if (!validate_filters(price, quantity, notional)) {
        return false;
    }

    mexc::QueryParams params = {
        {"timeInForce", "GTC"},
        {"quantity", format_decimal(quantity, config_.quantity_precision)},
        {"price", format_decimal(price, config_.price_precision)},
        {"newClientOrderId", client_order_id},
        {"recvWindow", "10000"}
    };

    try {
        const auto response = client_.new_order(config_.symbol, side, "LIMIT", std::move(params));
        const auto json = nlohmann::json::parse(response);

        std::string order_id;
        if (json.contains("orderId")) {
            if (json["orderId"].is_string()) {
                order_id = json["orderId"].get<std::string>();
            } else if (json["orderId"].is_number()) {
                order_id = std::to_string(json["orderId"].get<long long>());
            }
        }

        const auto status = get_string_optional(json, "status");
        if (!status.empty() && status != "NEW" && status != "PARTIALLY_FILLED") {
            std::cerr << "[Strategy] Limit order rejected with status " << status << std::endl;
            return false;
        }

        std::cout << "[Strategy] Placed " << side << " order id="
                  << (order_id.empty() ? client_order_id : order_id)
                  << " price=" << price << " qty=" << quantity << std::endl;
        return true;
    } catch (const mexc::HttpError& ex) {
        std::cerr << "[Strategy] Failed to place limit order: " << ex.what() << std::endl;
        if (ex.status_code() == 429) {
            note_rate_limit_hit();
        }
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "[Strategy] Failed to place limit order: " << ex.what() << std::endl;
        return false;
    }
}

bool MarketMakerStrategy::place_market_order(const std::string& side,
                                             double quantity,
                                             double quote_amount,
                                             const std::string& reason_tag) {
    mexc::QueryParams params = {
        {"recvWindow", "10000"}
    };

    if (side == "SELL") {
        double qty = floor_to_increment(quantity, config_.quantity_increment);
        if (qty < config_.min_base_quantity) {
            return false;
        }
        if (!validate_filters(0.0, qty, quote_amount)) {
            return false;
        }
        params.emplace_back("quantity", format_decimal(qty, config_.quantity_precision));
    } else if (side == "BUY") {
        double quote = floor_to_increment(std::max(quote_amount, config_.min_quote_order), config_.quote_increment);
        quote = std::min(quote, quote_balance_);
        if (quote < config_.min_quote_order) {
            return false;
        }
        if (!validate_filters(0.0, 0.0, quote)) {
            return false;
        }
        params.emplace_back("quoteOrderQty", format_decimal(quote, config_.quote_precision));
    } else {
        return false;
    }

    try {
        const auto response = client_.new_order(config_.symbol, side, "MARKET", std::move(params));
        const auto json = nlohmann::json::parse(response);
        std::cout << "[Strategy] Executed MARKET " << side << " (" << reason_tag << ") response="
                  << json.dump() << std::endl;
        return true;
    } catch (const mexc::HttpError& ex) {
        std::cerr << "[Strategy] Failed to place market order: " << ex.what() << std::endl;
        if (ex.status_code() == 429) {
            note_rate_limit_hit();
        }
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "[Strategy] Failed to place market order: " << ex.what() << std::endl;
        return false;
    }
}

bool MarketMakerStrategy::within_account_staleness(const std::chrono::system_clock::time_point& snapshot_time) const {
    if (config_.account_staleness_ms <= 0) {
        return true;
    }
    if (snapshot_time.time_since_epoch().count() == 0) {
        return false;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - snapshot_time).count();
    if (age_ms > config_.account_staleness_ms) {
        std::cerr << "[Strategy] Account snapshot stale (" << age_ms << " ms)" << std::endl;
        return false;
    }
    return true;
}

void MarketMakerStrategy::reset_risk_cooldown() {
    risk_disabled_since_ = std::chrono::system_clock::time_point{};
}

bool MarketMakerStrategy::throttle_taker_escape() {
    const auto now = std::chrono::system_clock::now();
    if (last_escape_time_.time_since_epoch().count() != 0) {
        const auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_escape_time_).count();
        if (delta_ms < config_.taker_escape_cooldown_ms) {
            std::cout << "[Risk] Escape throttled; last executed " << delta_ms << " ms ago." << std::endl;
            return false;
        }
    }

    if (escape_window_start_.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::seconds>(now - escape_window_start_) >= std::chrono::seconds(60)) {
        escape_window_start_ = now;
        escape_count_window_ = 0;
    }

    if (config_.max_taker_escapes_per_min > 0 &&
        escape_count_window_ >= config_.max_taker_escapes_per_min) {
        std::cout << "[Risk] Escape limit reached (" << escape_count_window_ << " per minute)" << std::endl;
        return false;
    }

    last_escape_time_ = now;
    ++escape_count_window_;
    return true;
}

bool MarketMakerStrategy::within_increment(double value, double increment) {
    if (increment <= kEpsilon) {
        return true;
    }
    const double steps = value / increment;
    return std::fabs(steps - std::round(steps)) < 1e-6;
}

bool MarketMakerStrategy::validate_filters(double price, double quantity, double notional) const {
    if (!symbol_filters_) {
        return true;
    }
    const auto& filters = *symbol_filters_;

    if (price > 0.0 && filters.tick_size > 0.0) {
        if (filters.min_price > 0.0 && price + kEpsilon < filters.min_price) {
            std::cerr << "[Filters] Price " << price << " below minimum " << filters.min_price << std::endl;
            return false;
        }
        if (!within_increment(price, filters.tick_size)) {
            std::cerr << "[Filters] Price " << price << " not aligned to tick size " << filters.tick_size << std::endl;
            return false;
        }
    }

    if (quantity > 0.0 && filters.step_size > 0.0) {
        if (filters.min_qty > 0.0 && quantity + kEpsilon < filters.min_qty) {
            std::cerr << "[Filters] Quantity " << quantity << " below minimum " << filters.min_qty << std::endl;
            return false;
        }
        if (!within_increment(quantity, filters.step_size)) {
            std::cerr << "[Filters] Quantity " << quantity << " not aligned to step size " << filters.step_size << std::endl;
            return false;
        }
    }

    if (notional > 0.0 && filters.min_notional > 0.0 && notional + kEpsilon < filters.min_notional) {
        std::cerr << "[Filters] Notional " << notional << " below minimum " << filters.min_notional << std::endl;
        return false;
    }

    return true;
}

void MarketMakerStrategy::note_rate_limit_hit() {
    rate_limited_this_loop_ = true;
    const auto now = std::chrono::steady_clock::now();
    if (current_backoff_ms_ <= 0.0) {
        current_backoff_ms_ = static_cast<double>(config_.rate_limit_backoff_ms_initial);
    } else {
        current_backoff_ms_ = std::min(current_backoff_ms_ * 1.5, static_cast<double>(config_.rate_limit_backoff_ms_max));
    }
    const auto backoff_duration = std::chrono::milliseconds(static_cast<int>(current_backoff_ms_));
    rate_limited_until_ = std::max(rate_limited_until_, now + backoff_duration);
    std::cout << "[RateLimit] Backing off for " << static_cast<int>(current_backoff_ms_) << " ms" << std::endl;
}

void MarketMakerStrategy::note_request_success() {
    if (rate_limited_this_loop_) {
        return;
    }
    if (current_backoff_ms_ > 0.0) {
        current_backoff_ms_ = std::max(0.0, current_backoff_ms_ * 0.5 - config_.rate_limit_backoff_ms_initial * 0.25);
        if (current_backoff_ms_ < config_.rate_limit_backoff_ms_initial * 0.5) {
            current_backoff_ms_ = 0.0;
            rate_limited_until_ = std::chrono::steady_clock::time_point{};
        }
    }
}

void MarketMakerStrategy::report_pnl(double nav, double base_share, bool first_iteration) {
    if (first_iteration || !initial_nav_) {
        initial_nav_ = nav;
        initial_base_ = base_balance_;
        initial_quote_ = quote_balance_;
        session_peak_nav_ = nav;
        realized_pnl_ = 0.0;
        std::cout << "[PNL] Initialized NAV=" << nav << std::endl;
        return;
    }

    const double pnl = nav - *initial_nav_;
    const double unrealized = pnl - realized_pnl_;
    std::cout << "[PNL] NAV=" << nav << " (Δ=" << pnl << ") base_share=" << base_share * 100.0
              << "% realized=" << realized_pnl_ << " unrealized=" << unrealized << std::endl;
}

bool MarketMakerStrategy::enforce_risk_limits(double nav, double base_share) {
    if (!initial_nav_) {
        session_peak_nav_ = nav;
        trading_enabled_ = true;
        reset_risk_cooldown();
        return true;
    }

    if (!session_peak_nav_ || nav > *session_peak_nav_) {
        session_peak_nav_ = nav;
    }

    double drawdown_abs = session_peak_nav_ ? (*session_peak_nav_ - nav) : 0.0;
    double drawdown_pct = (session_peak_nav_ && *session_peak_nav_ > kEpsilon)
        ? drawdown_abs / *session_peak_nav_
        : 0.0;

    const auto now = std::chrono::system_clock::now();

    if (trading_enabled_) {
        bool breach = false;
        if (config_.max_drawdown_usd > 0 && drawdown_abs > config_.max_drawdown_usd) {
            breach = true;
        }
        if (config_.max_drawdown_pct > 0 && drawdown_pct > config_.max_drawdown_pct) {
            breach = true;
        }
        if (breach) {
            trading_enabled_ = false;
            risk_disabled_since_ = now;
            std::cout << "[Risk] Drawdown exceeded thresholds (Δ=" << drawdown_abs
                      << ", " << drawdown_pct * 100.0 << "%). Disabling quoting." << std::endl;
            cancel_all_quotes();
        }
    } else {
        if (risk_disabled_since_.time_since_epoch().count() == 0) {
            risk_disabled_since_ = now;
        } else if (config_.risk_cooldown_ms > 0) {
            const auto disable_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - risk_disabled_since_).count();
            const double recovery_threshold = session_peak_nav_ ? *session_peak_nav_ * (1.0 - 0.5 * config_.max_drawdown_pct) : nav;
            if (disable_ms >= config_.risk_cooldown_ms && nav >= recovery_threshold) {
                trading_enabled_ = true;
                reset_risk_cooldown();
                std::cout << "[Risk] Cooldown elapsed; re-enabling quoting." << std::endl;
            }
        }
    }

    if (base_share > config_.max_inventory_ratio || base_share < (1.0 - config_.max_inventory_ratio)) {
        std::cout << "[Risk] Inventory imbalance: base_share=" << base_share * 100.0 << "%" << std::endl;
    }

    return trading_enabled_;
}

void MarketMakerStrategy::cancel_all_quotes() {
    try {
        client_.cancel_open_orders(config_.symbol);
    } catch (const mexc::HttpError& ex) {
        std::cerr << "[Risk] Failed to cancel open orders: " << ex.what() << std::endl;
        if (ex.status_code() == 429) {
            note_rate_limit_hit();
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Risk] Failed to cancel open orders: " << ex.what() << std::endl;
    }
}

double MarketMakerStrategy::compute_nav(const OrderBookSnapshot& book) const {
    const double mark = (book.microprice > kEpsilon) ? book.microprice : std::max(book.best_bid, book.best_ask);
    const double total_base = base_balance_ + base_locked_;
    const double total_quote = quote_balance_ + quote_locked_;
    return total_quote + total_base * mark;
}

double MarketMakerStrategy::compute_base_share(double nav, const OrderBookSnapshot& book) const {
    if (nav <= kEpsilon) {
        return 0.0;
    }
    const double mark = (book.microprice > kEpsilon) ? book.microprice : std::max(book.best_bid, book.best_ask);
    const double total_base = base_balance_ + base_locked_;
    return (total_base * mark) / nav;
}

} // namespace strategy
