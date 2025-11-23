#include "mexc/http_client.hpp"

#include <sstream>
#include <utility>

namespace mexc {
namespace {

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<const char*>(contents), size * nmemb);
    return size * nmemb;
}

} // namespace

HttpClient::HttpClient()
    : global_initialized_(false) {
    const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        throw HttpError("Failed to initialize libcurl: " + std::string(curl_easy_strerror(code)));
    }
    global_initialized_ = true;
}

HttpClient::~HttpClient() {
    if (global_initialized_) {
        curl_global_cleanup();
    }
}

RequestTimings HttpClient::collect_timings(CURL* handle) const {
    RequestTimings timings;
    double value = 0.0;

    if (curl_easy_getinfo(handle, CURLINFO_NAMELOOKUP_TIME, &value) == CURLE_OK) {
        timings.name_lookup_ms = value * 1000.0;
    }
    if (curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME, &value) == CURLE_OK) {
        timings.connect_ms = value * 1000.0;
    }
    if (curl_easy_getinfo(handle, CURLINFO_APPCONNECT_TIME, &value) == CURLE_OK) {
        timings.app_connect_ms = value * 1000.0;
    }
    if (curl_easy_getinfo(handle, CURLINFO_PRETRANSFER_TIME, &value) == CURLE_OK) {
        timings.pre_transfer_ms = value * 1000.0;
    }
    if (curl_easy_getinfo(handle, CURLINFO_STARTTRANSFER_TIME, &value) == CURLE_OK) {
        timings.start_transfer_ms = value * 1000.0;
    }
    if (curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME, &value) == CURLE_OK) {
        timings.total_ms = value * 1000.0;
    }

    return timings;
}

HttpResponse HttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) const {
    CURL* handle = curl_easy_init();
    if (!handle) {
        throw HttpError("Failed to create CURL easy handle");
    }

    std::string response_body;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 15L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        header_list = curl_slist_append(header_list, (header.first + ": " + header.second).c_str());
    }

    if (header_list) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list);
    }

    if (!body.empty()) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const auto perform_code = curl_easy_perform(handle);

    if (header_list) {
        curl_slist_free_all(header_list);
    }

    long status_code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code);

    if (perform_code != CURLE_OK) {
        curl_easy_cleanup(handle);
        throw HttpError("libcurl request failed: " + std::string(curl_easy_strerror(perform_code)));
    }

    if (status_code >= 400) {
        curl_easy_cleanup(handle);
        throw HttpError("HTTP error: " + response_body, status_code);
    }

    HttpResponse response{status_code, std::move(response_body), collect_timings(handle)};
    curl_easy_cleanup(handle);
    return response;
}

} // namespace mexc
