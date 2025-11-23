#include "mexc/client_base.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mexc {
namespace {

long current_timestamp_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string hmac_sha256_hex(const std::string& key, const std::string& message) {
    unsigned int len = 0;
    unsigned char buffer[EVP_MAX_MD_SIZE];

    const unsigned char* digest = HMAC(
        EVP_sha256(),
        key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(message.data()), message.size(),
        buffer,
        &len);

    if (digest == nullptr) {
        throw std::runtime_error("Failed to create HMAC signature");
    }

    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(buffer[i]);
    }

    return oss.str();
}

} // namespace

ClientBase::ClientBase(Credentials credentials, std::string base_url)
    : credentials_(std::move(credentials)),
      base_url_(std::move(base_url)),
      http_client_(),
      last_timings_{} {}

RequestTimings ClientBase::last_request_timings() const {
    return last_timings_;
}

HttpResponse ClientBase::public_request(
    const std::string& method,
    const std::string& path,
    const QueryParams& params) const {

    std::string url = base_url_ + path;
    const auto query = build_query_string(params);
    if (!query.empty()) {
        url += '?' + query;
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}
    };

    if (!credentials_.api_key.empty()) {
        headers.emplace_back("X-MEXC-APIKEY", credentials_.api_key);
    }

    auto response = http_client_.request(method, url, headers);
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        last_timings_ = response.timings;
    }
    return response;
}

HttpResponse ClientBase::signed_request(
    const std::string& method,
    const std::string& path,
    QueryParams params) const {

    if (credentials_.api_key.empty() || credentials_.api_secret.empty()) {
        throw std::invalid_argument("API key and secret are required for signed requests");
    }

    const auto signed_query = build_signed_query(std::move(params));
    std::string url = base_url_ + path + '?' + signed_query;

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"X-MEXC-APIKEY", credentials_.api_key}
    };

    auto response = http_client_.request(method, url, headers);
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        last_timings_ = response.timings;
    }
    return response;
}

std::string ClientBase::build_signed_query(QueryParams params) const {
    params.emplace_back("timestamp", std::to_string(current_timestamp_ms()));
    const auto filtered = filter_empty(params);
    const auto query = build_query_string(filtered);
    const auto signature = hmac_sha256_hex(credentials_.api_secret, query);
    return query + "&signature=" + signature;
}

} // namespace mexc
