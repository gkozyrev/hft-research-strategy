#pragma once

#include <curl/curl.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mexc {

using QueryParams = std::vector<std::pair<std::string, std::string>>;

std::string url_encode(const std::string& value);

QueryParams filter_empty(const QueryParams& params);

std::string build_query_string(const QueryParams& params);

std::string to_upper_copy(std::string value);

} // namespace mexc
