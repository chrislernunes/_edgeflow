#pragma once
#include "signal_engine.h"
#include "signal_validator.h"
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// SimTrade — one completed round-trip
// ─────────────────────────────────────────────────────────────────────────────

struct SimTrade {
    uint64_t entry_ts_us         = 0;
    uint64_t exit_ts_us          = 0;
    double   entry_price         = 0.0;
    double   exit_price          = 0.0;
    double   signal_at_entry     = 0.0;
    double   spread_at_entry_bps = 0.0;
    bool     is_long             = false;
    double   pnl_bps             = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ExecutionSimulatorConfig — hoisted outside class (MinGW fix)
// ─────────────────────────────────────────────────────────────────────────────

struct ExecutionSimulatorConfig {
    bool     passive_entry   = false;    // false = cross spread
    double   min_signal      = 0.20;     // |composite| threshold to enter
    uint64_t hold_us         = 250000u;  // hold before flat exit
    uint64_t fill_timeout_us = 100000u;  // passive fill timeout
    double   maker_fee_bps   = 0.2;      // per side
    double   taker_fee_bps   = 0.5;      // per side
    double   max_spread_bps  = 3.0;      // skip if spread wider
    int      max_inventory   = 1;        // max simultaneous positions
};

// ─────────────────────────────────────────────────────────────────────────────
// ExecutionSimulator
// ─────────────────────────────────────────────────────────────────────────────

class ExecutionSimulator {
public:
    using Config = ExecutionSimulatorConfig;

    explicit ExecutionSimulator(Config cfg = Config{});

    void on_signal(const SignalSnapshot& snap);
    void update(uint64_t now_us, double mid, double best_bid, double best_ask);

    std::string report_string() const;
    void        print_report()  const;

    const std::vector<SimTrade>& trades() const { return trades_; }

private:
    Config cfg_;

    struct OpenPosition {
        uint64_t entry_ts_us      = 0;
        uint64_t fill_deadline_us = 0;
        double   entry_price      = 0.0;
        double   signal_value     = 0.0;
        double   spread_bps       = 0.0;
        bool     is_long          = false;
        bool     filled           = false;
        bool     passive          = false;
    };

    std::deque<OpenPosition> open_;
    std::vector<SimTrade>    trades_;

    double   total_pnl_bps_ = 0.0;
    uint64_t n_trades_      = 0;
    uint64_t n_wins_        = 0;
    uint64_t n_expired_     = 0;

    bool   can_enter() const { return (int)open_.size() < cfg_.max_inventory; }
    double fee_bps(bool passive) const {
        return passive ? cfg_.maker_fee_bps * 2.0 : cfg_.taker_fee_bps * 2.0;
    }
};

} // namespace hft
