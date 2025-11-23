#include "mexc/spot_client.hpp"
#include "strategy/market_maker.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void load_env_file(const std::string& path) {
    std::ifstream env_file(path);
    if (!env_file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(env_file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        auto key = trim(line.substr(0, pos));
        auto value = trim(line.substr(pos + 1));

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            setenv(key.c_str(), value.c_str(), 1);
        }
    }
}

mexc::Credentials load_credentials_from_env() {
    const char* api_key = std::getenv("MEXC_API_KEY");
    const char* api_secret = std::getenv("MEXC_API_SECRET");
    return mexc::Credentials{api_key ? api_key : "", api_secret ? api_secret : ""};
}

int main() {
    load_env_file(".env");

    const auto credentials = load_credentials_from_env();
    mexc::SpotClient client{credentials};

    const auto server_time = client.server_time();
    const auto timings = client.last_request_timings();
    std::cout << "MEXC connectivity check -> server time: " << server_time << std::endl;
    std::cout << "REST latency: total=" << timings.total_ms << " ms"
              << ", connect=" << timings.connect_ms << " ms"
              << ", tls=" << timings.app_connect_ms << " ms" << std::endl;

    strategy::MarketMakerConfig config;
    config.symbol = "SPYXUSDT";
    config.quote_budget = 5.0;
    config.min_quote_order = 1.05;
    config.min_base_quantity = 0.002;
    config.spread_bps = 15.0;
    config.min_edge_bps = 8.0;
    config.inventory_tolerance = 0.10;
    config.max_inventory_ratio = 0.75;
    config.escape_bps = 25.0;
    config.refresh_interval_ms = 1000;
    config.quantity_increment = 0.0001;
    config.quote_increment = 0.01;
    config.quantity_precision = 4;
    config.quote_precision = 2;
    config.max_drawdown_pct = 0.15;
    config.max_drawdown_usd = 8.0;

    strategy::MarketMakerStrategy strategy{client, config};
    strategy.run();

    return 0;
}
