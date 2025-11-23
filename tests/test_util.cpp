#include "mexc/util.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("url_encode handles safe and unsafe characters") {
    using mexc::url_encode;
    CHECK(url_encode("simple") == "simple");
    CHECK(url_encode("hello world") == "hello%20world");
    CHECK(url_encode("1+1=2") == "1%2B1%3D2");
    CHECK(url_encode("symbols-_.~") == "symbols-_.~");
}

TEST_CASE("filter_empty removes empty values") {
    using mexc::filter_empty;
    mexc::QueryParams params = {
        {"key1", "value"},
        {"key2", ""},
        {"key3", "0"},
        {"key4", "false"}
    };

    const auto filtered = filter_empty(params);
    REQUIRE(filtered.size() == 3);
    CHECK(filtered[0].first == "key1");
    CHECK(filtered[1].first == "key3");
    CHECK(filtered[2].first == "key4");
}

TEST_CASE("build_query_string preserves order and encodes values") {
    using mexc::build_query_string;
    mexc::QueryParams params = {
        {"symbol", "btc_usdt"},
        {"limit", "100"},
        {"note", "space value"}
    };

    CHECK(build_query_string(params) == "symbol=btc_usdt&limit=100&note=space%20value");
}

TEST_CASE("to_upper_copy converts strings to uppercase") {
    using mexc::to_upper_copy;
    CHECK(to_upper_copy("btcUSDT") == "BTCUSDT");
    CHECK(to_upper_copy("already upper") == "ALREADY UPPER");
}
