#pragma once

#include "mexc/http_client.hpp"
#include "mexc/util.hpp"

#include <openssl/hmac.h>

#include <chrono>
#include <mutex>
#include <string>

namespace mexc {

struct Credentials {
    std::string api_key;
    std::string api_secret;
};

class ClientBase {
public:
    explicit ClientBase(Credentials credentials,
                        std::string base_url = "https://api.mexc.com/api/v3");

    [[nodiscard]] RequestTimings last_request_timings() const;

protected:
    HttpResponse public_request(
        const std::string& method,
        const std::string& path,
        const QueryParams& params = {}) const;

    HttpResponse signed_request(
        const std::string& method,
        const std::string& path,
        QueryParams params = {}) const;

private:
    std::string build_signed_query(QueryParams params) const;

    Credentials credentials_;
    std::string base_url_;
    HttpClient http_client_;
    mutable RequestTimings last_timings_;
    mutable std::mutex request_mutex_;
};

} // namespace mexc
