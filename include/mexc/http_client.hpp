#pragma once

#include <curl/curl.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mexc {

struct RequestTimings {
    double name_lookup_ms = 0.0;
    double connect_ms = 0.0;
    double app_connect_ms = 0.0;
    double pre_transfer_ms = 0.0;
    double start_transfer_ms = 0.0;
    double total_ms = 0.0;
};

struct HttpResponse {
    long status_code;
    std::string body;
    RequestTimings timings;
};

class HttpError : public std::runtime_error {
public:
    explicit HttpError(const std::string& message, long status_code = 0)
        : std::runtime_error(message), status_code_(status_code) {}

    [[nodiscard]] long status_code() const noexcept { return status_code_; }

private:
    long status_code_;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept = delete;
    HttpClient& operator=(HttpClient&&) noexcept = delete;

    HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers = {},
        const std::string& body = ""
    ) const;

private:
    RequestTimings collect_timings(CURL* handle) const;

    bool global_initialized_;
};

} // namespace mexc
