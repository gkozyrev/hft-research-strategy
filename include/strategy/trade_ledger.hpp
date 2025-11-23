#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace strategy {

enum class TradeSide { Buy, Sell };

struct TradeLedgerConfig {
    std::filesystem::path storage_path;
    int base_scale = 10000;   // derived from quantity precision
    int quote_scale = 100;    // derived from quote precision
};

struct TradeFill {
    long long id = 0;
    std::chrono::system_clock::time_point timestamp{};
    TradeSide side = TradeSide::Buy;
    int64_t base_qty = 0;     // scaled integer units
    int64_t quote_qty = 0;    // scaled integer units
    int64_t fee_qty = 0;      // scaled to fee asset precision
    std::string fee_asset;
    bool is_maker = false;
};

struct LedgerState {
    int64_t position_base = 0;
    int64_t position_cost = 0;   // quote units
    int64_t realized_pnl = 0;    // quote units
    long long last_trade_id = 0;
};

class TradeLedger {
public:
    explicit TradeLedger(TradeLedgerConfig config);

    LedgerState load();
    void append(const TradeFill& fill);
    const LedgerState& state() const { return state_; }

private:
    void ensure_directory() const;
    void rebuild_from_entries();
    void persist_fill(const TradeFill& fill);

    TradeLedgerConfig config_;
    LedgerState state_{};
    std::vector<TradeFill> entries_;
};

} // namespace strategy


