#include "mexc/spot_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void load_env_file(const std::string& name) {
    static bool loaded = false;
    if (loaded) {
        return;
    }

    const std::filesystem::path source_root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::filesystem::path candidates[] = {
        std::filesystem::current_path() / name,
        source_root / name
    };

    std::ifstream env_file;
    for (const auto& candidate : candidates) {
        env_file.open(candidate);
        if (env_file.is_open()) {
            break;
        }
        env_file.clear();
    }

    if (!env_file.is_open()) {
        loaded = true;
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

    loaded = true;
}

mexc::Credentials load_credentials() {
    load_env_file(".env");
    const char* api_key = std::getenv("MEXC_API_KEY");
    const char* api_secret = std::getenv("MEXC_API_SECRET");
    return mexc::Credentials{api_key ? api_key : "", api_secret ? api_secret : ""};
}

void log_timings(const std::string& label, const mexc::RequestTimings& timings) {
    std::cout << "[MEXC] " << label
              << " total=" << timings.total_ms << " ms"
              << ", connect=" << timings.connect_ms << " ms"
              << ", tls=" << timings.app_connect_ms << " ms"
              << ", start_transfer=" << timings.start_transfer_ms << " ms"
              << std::endl;
}

} // namespace

TEST_CASE("SpotClient depth retrieves live order book", "[integration][mexc]") {
    const auto credentials = load_credentials();
    mexc::SpotClient client{credentials};

    try {
        const auto response = client.depth("BTCUSDT", 5);
        const auto timings = client.last_request_timings();
        log_timings("order book", timings);

        REQUIRE_FALSE(response.empty());
        CHECK(response.find("\"bids\"") != std::string::npos);
        CHECK(response.find("\"asks\"") != std::string::npos);
    } catch (const mexc::HttpError& ex) {
        FAIL_CHECK("HTTP error while fetching order book: " << ex.what());
    } catch (const std::exception& ex) {
        FAIL_CHECK("Unexpected error while fetching order book: " << ex.what());
    }
}

TEST_CASE("SpotClient account_info retrieves balances", "[integration][mexc]") {
    const auto credentials = load_credentials();
    if (credentials.api_key.empty() || credentials.api_secret.empty()) {
        WARN("MEXC credentials not provided; skipping account_info integration test");
        return;
    }

    mexc::SpotClient client{credentials};

    try {
        const auto response = client.account_info();
        const auto timings = client.last_request_timings();
        log_timings("account info", timings);

        REQUIRE_FALSE(response.empty());
        CHECK(response.find("\"balances\"") != std::string::npos);
    } catch (const mexc::HttpError& ex) {
        // Check for IP whitelist errors - these are configuration issues, not code bugs
        const std::string error_msg = ex.what();
        if (error_msg.find("not in the ip white list") != std::string::npos ||
            error_msg.find("IP") != std::string::npos && error_msg.find("white list") != std::string::npos) {
            WARN("IP address not whitelisted in MEXC account; skipping account_info test. "
                 "Add your IP to the whitelist in MEXC account settings to enable this test.");
            return;
        }
        FAIL_CHECK("HTTP error while fetching account info: " << ex.what());
    } catch (const std::exception& ex) {
        FAIL_CHECK("Unexpected error while fetching account info: " << ex.what());
    }
}

