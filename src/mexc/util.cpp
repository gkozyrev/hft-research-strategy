#include "mexc/util.hpp"

#include <cctype>

namespace mexc {

std::string url_encode(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped.push_back(static_cast<char>(c));
        } else {
            escaped.push_back('%');
            constexpr char hex_chars[] = "0123456789ABCDEF";
            escaped.push_back(hex_chars[(c >> 4) & 0x0F]);
            escaped.push_back(hex_chars[c & 0x0F]);
        }
    }
    return escaped;
}

QueryParams filter_empty(const QueryParams& params) {
    QueryParams filtered;
    filtered.reserve(params.size());
    for (const auto& [key, value] : params) {
        if (!value.empty()) {
            filtered.emplace_back(key, value);
        }
    }
    return filtered;
}

std::string build_query_string(const QueryParams& params) {
    const auto filtered = filter_empty(params);
    std::ostringstream oss;
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        if (i != 0) {
            oss << '&';
        }
        oss << url_encode(filtered[i].first) << '=' << url_encode(filtered[i].second);
    }
    return oss.str();
}

std::string to_upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

} // namespace mexc
