#pragma once

#include "mexc/client_base.hpp"

#include <optional>

namespace mexc {

class SpotClient : public ClientBase {
public:
    explicit SpotClient(Credentials credentials,
                        std::string base_url = "https://api.mexc.com/api/v3");

    std::string ping() const;
    std::string server_time() const;

    std::string exchange_info(std::optional<std::string> symbol = std::nullopt,
                              std::optional<std::string> symbols = std::nullopt) const;

    std::string depth(const std::string& symbol,
                      std::optional<int> limit = std::nullopt) const;

    std::string trades(const std::string& symbol,
                       std::optional<int> limit = std::nullopt) const;

    std::string historical_trades(const std::string& symbol,
                                  std::optional<int> limit = std::nullopt,
                                  std::optional<int> from_id = std::nullopt) const;

    std::string agg_trades(const std::string& symbol,
                           QueryParams options = {}) const;

    std::string klines(const std::string& symbol,
                       const std::string& interval,
                       QueryParams options = {}) const;

    std::string avg_price(const std::string& symbol) const;

    std::string ticker_24hr(std::optional<std::string> symbol = std::nullopt) const;
    std::string ticker_price(std::optional<std::string> symbol = std::nullopt) const;
    std::string book_ticker(std::optional<std::string> symbol = std::nullopt) const;

    std::string account_info() const;
    std::string account_trade_list(const std::string& symbol,
                                   QueryParams options = {}) const;

    std::string new_order_test(const std::string& symbol,
                               const std::string& side,
                               const std::string& type,
                               QueryParams options = {}) const;

    std::string new_order(const std::string& symbol,
                          const std::string& side,
                          const std::string& type,
                          QueryParams options = {}) const;

    std::string cancel_order(const std::string& symbol, QueryParams options = {}) const;
    std::string cancel_open_orders(const std::string& symbol) const;
    std::string query_order(const std::string& symbol, QueryParams options = {}) const;
    std::string open_orders(const std::string& symbol) const;
    std::string all_orders(const std::string& symbol, QueryParams options = {}) const;

    // User data stream methods
    std::string create_listen_key() const;
    std::string extend_listen_key(const std::string& listen_key) const;
    std::string delete_listen_key(const std::string& listen_key) const;
};

} // namespace mexc
