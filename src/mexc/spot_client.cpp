#include "mexc/spot_client.hpp"

#include <sstream>

namespace mexc {
namespace {

template <typename T>
void add_optional(QueryParams& params, const std::string& key, const std::optional<T>& value) {
    if (value.has_value()) {
        std::ostringstream oss;
        oss << *value;
        params.emplace_back(key, oss.str());
    }
}

QueryParams merge_params(QueryParams base, QueryParams extra) {
    base.reserve(base.size() + extra.size());
    for (auto& pair : extra) {
        base.emplace_back(std::move(pair));
    }
    return base;
}

} // namespace

SpotClient::SpotClient(Credentials credentials, std::string base_url)
    : ClientBase(std::move(credentials), std::move(base_url)) {}

std::string SpotClient::ping() const {
    return public_request("GET", "/ping").body;
}

std::string SpotClient::server_time() const {
    return public_request("GET", "/time").body;
}

std::string SpotClient::exchange_info(std::optional<std::string> symbol,
                                      std::optional<std::string> symbols) const {
    QueryParams params;
    if (symbol) {
        params.emplace_back("symbol", to_upper_copy(*symbol));
    }
    if (symbols) {
        std::string value = *symbols;
        for (auto& ch : value) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        params.emplace_back("symbols", value);
    }
    return public_request("GET", "/exchangeInfo", std::move(params)).body;
}

std::string SpotClient::depth(const std::string& symbol, std::optional<int> limit) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    add_optional(params, "limit", limit);
    return public_request("GET", "/depth", std::move(params)).body;
}

std::string SpotClient::trades(const std::string& symbol, std::optional<int> limit) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    add_optional(params, "limit", limit);
    return public_request("GET", "/trades", std::move(params)).body;
}

std::string SpotClient::historical_trades(const std::string& symbol,
                                          std::optional<int> limit,
                                          std::optional<int> from_id) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    add_optional(params, "limit", limit);
    add_optional(params, "fromId", from_id);
    return public_request("GET", "/historicalTrades", std::move(params)).body;
}

std::string SpotClient::agg_trades(const std::string& symbol, QueryParams options) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    params = merge_params(std::move(params), std::move(options));
    return public_request("GET", "/aggTrades", std::move(params)).body;
}

std::string SpotClient::klines(const std::string& symbol,
                               const std::string& interval,
                               QueryParams options) const {
    QueryParams params = {
        {"symbol", to_upper_copy(symbol)},
        {"interval", interval}
    };
    params = merge_params(std::move(params), std::move(options));
    return public_request("GET", "/klines", std::move(params)).body;
}

std::string SpotClient::avg_price(const std::string& symbol) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    return public_request("GET", "/avgPrice", std::move(params)).body;
}

std::string SpotClient::ticker_24hr(std::optional<std::string> symbol) const {
    QueryParams params;
    if (symbol) {
        params.emplace_back("symbol", to_upper_copy(*symbol));
    }
    return public_request("GET", "/ticker/24hr", std::move(params)).body;
}

std::string SpotClient::ticker_price(std::optional<std::string> symbol) const {
    QueryParams params;
    if (symbol) {
        params.emplace_back("symbol", to_upper_copy(*symbol));
    }
    return public_request("GET", "/ticker/price", std::move(params)).body;
}

std::string SpotClient::book_ticker(std::optional<std::string> symbol) const {
    QueryParams params;
    if (symbol) {
        params.emplace_back("symbol", to_upper_copy(*symbol));
    }
    return public_request("GET", "/ticker/bookTicker", std::move(params)).body;
}

std::string SpotClient::account_info() const {
    return signed_request("GET", "/account").body;
}

std::string SpotClient::account_trade_list(const std::string& symbol, QueryParams options) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    params = merge_params(std::move(params), std::move(options));
    return signed_request("GET", "/myTrades", std::move(params)).body;
}

std::string SpotClient::new_order_test(const std::string& symbol,
                                       const std::string& side,
                                       const std::string& type,
                                       QueryParams options) const {
    QueryParams params = {
        {"symbol", to_upper_copy(symbol)},
        {"side", to_upper_copy(side)},
        {"type", to_upper_copy(type)}
    };
    params = merge_params(std::move(params), std::move(options));
    return signed_request("POST", "/order/test", std::move(params)).body;
}

std::string SpotClient::new_order(const std::string& symbol,
                                  const std::string& side,
                                  const std::string& type,
                                  QueryParams options) const {
    QueryParams params = {
        {"symbol", to_upper_copy(symbol)},
        {"side", to_upper_copy(side)},
        {"type", to_upper_copy(type)}
    };
    params = merge_params(std::move(params), std::move(options));
    return signed_request("POST", "/order", std::move(params)).body;
}

std::string SpotClient::cancel_order(const std::string& symbol, QueryParams options) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    params = merge_params(std::move(params), std::move(options));
    return signed_request("DELETE", "/order", std::move(params)).body;
}

std::string SpotClient::cancel_open_orders(const std::string& symbol) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    return signed_request("DELETE", "/openOrders", std::move(params)).body;
}

std::string SpotClient::query_order(const std::string& symbol, QueryParams options) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    params = merge_params(std::move(params), std::move(options));
    return signed_request("GET", "/order", std::move(params)).body;
}

std::string SpotClient::open_orders(const std::string& symbol) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    return signed_request("GET", "/openOrders", std::move(params)).body;
}

std::string SpotClient::all_orders(const std::string& symbol, QueryParams options) const {
    QueryParams params = {{"symbol", to_upper_copy(symbol)}};
    params = merge_params(std::move(params), std::move(options));
    return signed_request("GET", "/allOrders", std::move(params)).body;
}

std::string SpotClient::create_listen_key() const {
    return signed_request("POST", "/userDataStream").body;
}

std::string SpotClient::extend_listen_key(const std::string& listen_key) const {
    QueryParams params = {{"listenKey", listen_key}};
    return signed_request("PUT", "/userDataStream", std::move(params)).body;
}

std::string SpotClient::delete_listen_key(const std::string& listen_key) const {
    QueryParams params = {{"listenKey", listen_key}};
    return signed_request("DELETE", "/userDataStream", std::move(params)).body;
}

} // namespace mexc
