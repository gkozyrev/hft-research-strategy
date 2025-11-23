#include "strategy/trade_ledger.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace strategy {

namespace {

constexpr int64_t kQuoteCapacityLimit = static_cast<int64_t>(1e15);

int64_t safe_add(int64_t lhs, int64_t rhs) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
        throw std::overflow_error("Ledger integer overflow");
    }
    return lhs + rhs;
}

template <typename T>
T json_value_or(const nlohmann::json& j, const char* key, T fallback) {
    if (!j.contains(key)) {
        return fallback;
    }
    try {
        return j[key].get<T>();
    } catch (...) {
        return fallback;
    }
}

} // namespace

TradeLedger::TradeLedger(TradeLedgerConfig config)
    : config_(std::move(config)) {
    if (config_.base_scale <= 0 || config_.quote_scale <= 0) {
        throw std::invalid_argument("TradeLedger scales must be positive");
    }
}

LedgerState TradeLedger::load() {
    entries_.clear();
    state_ = {};

    ensure_directory();
    std::ifstream input(config_.storage_path);
    if (!input.good()) {
        return state_;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const auto json = nlohmann::json::parse(line);
            TradeFill fill;
            fill.id = json_value_or<long long>(json, "id", 0);
            const auto epoch_ms = json_value_or<int64_t>(json, "time", 0);
            fill.timestamp = std::chrono::system_clock::time_point{std::chrono::milliseconds(epoch_ms)};
            fill.side = json_value_or<std::string>(json, "side", "BUY") == "SELL"
                        ? TradeSide::Sell
                        : TradeSide::Buy;
            fill.base_qty = json_value_or<int64_t>(json, "base", 0);
            fill.quote_qty = json_value_or<int64_t>(json, "quote", 0);
            fill.fee_qty = json_value_or<int64_t>(json, "feeQty", 0);
            fill.fee_asset = json_value_or<std::string>(json, "feeAsset", "");
            fill.is_maker = json_value_or<bool>(json, "isMaker", true);
            entries_.push_back(fill);
        } catch (...) {
            continue;
        }
    }

    rebuild_from_entries();
    return state_;
}

void TradeLedger::append(const TradeFill& fill) {
    entries_.push_back(fill);
    persist_fill(fill);

    if (fill.side == TradeSide::Buy) {
        state_.position_base = safe_add(state_.position_base, fill.base_qty);
        state_.position_cost = safe_add(state_.position_cost, fill.quote_qty);
    } else {
        int64_t remaining = fill.base_qty;
        while (remaining > 0 && state_.position_base > 0) {
            const double avg_cost = static_cast<double>(state_.position_cost) /
                                    static_cast<double>(std::max<int64_t>(state_.position_base, 1));
            const int64_t matched = std::min(state_.position_base, remaining);
            const int64_t cost_reduction = static_cast<int64_t>(std::llround(avg_cost * static_cast<double>(matched)));
            const double fill_ratio = static_cast<double>(matched) / static_cast<double>(fill.base_qty);
            const int64_t proceeds = static_cast<int64_t>(std::llround(fill.quote_qty * fill_ratio));

            state_.position_base -= matched;
            state_.position_cost = std::max<int64_t>(0, state_.position_cost - cost_reduction);
            state_.realized_pnl = safe_add(state_.realized_pnl, proceeds - cost_reduction);

            remaining -= matched;
        }
    }

    state_.last_trade_id = std::max(state_.last_trade_id, fill.id);
    state_.realized_pnl = std::clamp(state_.realized_pnl, -kQuoteCapacityLimit, kQuoteCapacityLimit);
}

void TradeLedger::ensure_directory() const {
    if (config_.storage_path.empty()) {
        throw std::runtime_error("TradeLedger storage path not set");
    }
    const auto dir = config_.storage_path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }
}

void TradeLedger::rebuild_from_entries() {
    state_ = {};
    std::vector<TradeFill> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(), [](const TradeFill& a, const TradeFill& b) {
        return a.id < b.id;
    });

    for (const auto& fill : sorted) {
        if (fill.side == TradeSide::Buy) {
            state_.position_base = safe_add(state_.position_base, fill.base_qty);
            state_.position_cost = safe_add(state_.position_cost, fill.quote_qty);
        } else {
            int64_t remaining = fill.base_qty;
            while (remaining > 0 && state_.position_base > 0) {
                const double avg_cost = static_cast<double>(state_.position_cost) /
                                        static_cast<double>(std::max<int64_t>(state_.position_base, 1));
                const int64_t matched = std::min(state_.position_base, remaining);
                const int64_t cost_reduction = static_cast<int64_t>(std::llround(avg_cost * static_cast<double>(matched)));
                const double fill_ratio = static_cast<double>(matched) / static_cast<double>(fill.base_qty);
                const int64_t proceeds = static_cast<int64_t>(std::llround(fill.quote_qty * fill_ratio));

                state_.position_base -= matched;
                state_.position_cost = std::max<int64_t>(0, state_.position_cost - cost_reduction);
                state_.realized_pnl = safe_add(state_.realized_pnl, proceeds - cost_reduction);

                remaining -= matched;
            }
        }

        state_.last_trade_id = std::max(state_.last_trade_id, fill.id);
    }
    state_.realized_pnl = std::clamp(state_.realized_pnl, -kQuoteCapacityLimit, kQuoteCapacityLimit);
}

void TradeLedger::persist_fill(const TradeFill& fill) {
    ensure_directory();

    nlohmann::json json;
    json["id"] = fill.id;
    json["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        fill.timestamp.time_since_epoch()).count();
    json["side"] = (fill.side == TradeSide::Buy) ? "BUY" : "SELL";
    json["base"] = fill.base_qty;
    json["quote"] = fill.quote_qty;
    json["feeQty"] = fill.fee_qty;
    json["feeAsset"] = fill.fee_asset;
    json["isMaker"] = fill.is_maker;

    std::ofstream output(config_.storage_path, std::ios::app);
    if (!output.good()) {
        throw std::runtime_error("Failed to append to trade ledger at " + config_.storage_path.string());
    }
    output << json.dump() << '\n';
}

} // namespace strategy


