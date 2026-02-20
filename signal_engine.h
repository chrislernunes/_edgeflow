#pragma once
#include "order_book.h"
#include <cstdint>
#include <deque>
#include <string>
#include <optional>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// SignalSnapshot — output of SignalEngine::compute()
// ─────────────────────────────────────────────────────────────────────────────

struct SignalSnapshot {
    uint64_t ts_us          = 0;
    uint64_t exchange_ts_ms = 0;

    double best_bid   = 0.0;
    double best_ask   = 0.0;
    double mid        = 0.0;
    double spread     = 0.0;
    double spread_bps = 0.0;

    double microprice            = 0.0;
    double microprice_offset_bps = 0.0;

    double obi_L1  = 0.0;
    double obi_L3  = 0.0;
    double obi_L5  = 0.0;
    double obi_L10 = 0.0;

    double buy_volume  = 0.0;
    double sell_volume = 0.0;
    double trade_flow  = 0.0;
    double vwap_buy    = 0.0;
    double vwap_sell   = 0.0;
    int    trade_count = 0;

    double composite = 0.0;

    enum class SpreadRegime { TIGHT, NORMAL, WIDE };
    SpreadRegime spread_regime = SpreadRegime::NORMAL;

    std::string spread_regime_str() const {
        switch (spread_regime) {
        case SpreadRegime::TIGHT:  return "TIGHT";
        case SpreadRegime::NORMAL: return "NORMAL";
        case SpreadRegime::WIDE:   return "WIDE";
        }
        return "?";
    }

    std::string to_string() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// TradeEvent — from aggTrade stream
// ─────────────────────────────────────────────────────────────────────────────

struct TradeEvent {
    uint64_t ts_us          = 0;
    double   price          = 0.0;
    double   qty            = 0.0;
    bool     is_buyer_maker = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// SignalEngineConfig — hoisted outside class (MinGW default-arg fix)
// ─────────────────────────────────────────────────────────────────────────────

struct SignalEngineConfig {
    uint64_t flow_window_us   = 500000u;
    double   tight_spread_bps = 1.0;
    double   wide_spread_bps  = 5.0;
    double   w_obi_l1         = 0.30;
    double   w_obi_l5         = 0.20;
    double   w_microprice     = 0.20;
    double   w_trade_flow     = 0.30;
    double   min_flow_volume  = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SignalFlowStats — hoisted outside class (MinGW default-arg fix)
// ─────────────────────────────────────────────────────────────────────────────

struct SignalFlowStats {
    double   buy_volume  = 0.0;
    double   sell_volume = 0.0;
    double   trade_flow  = 0.0;
    double   vwap_buy    = 0.0;
    double   vwap_sell   = 0.0;
    int      count       = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SignalEngine
// ─────────────────────────────────────────────────────────────────────────────

class SignalEngine {
public:
    using Config    = SignalEngineConfig;
    using FlowStats = SignalFlowStats;

    explicit SignalEngine(Config cfg = Config{});

    SignalSnapshot compute(const OrderBook& book,
                           uint64_t local_ts_us,
                           uint64_t exchange_ts_ms = 0);

    void on_trade(const TradeEvent& trade);

    size_t        trade_window_size()                  const { return trades_.size(); }
    const Config& config()                             const { return cfg_; }
    FlowStats     current_flow_stats(uint64_t now_us)  const;

private:
    Config cfg_;

    struct TradeRecord {
        uint64_t ts_us  = 0;
        double   price  = 0.0;
        double   qty    = 0.0;
        bool     is_buy = false;
    };

    std::deque<TradeRecord> trades_;

    void         evict_old_trades(uint64_t now_us);
    double       compute_composite(const SignalSnapshot& s) const;
    SignalSnapshot::SpreadRegime classify_spread(double spread_bps) const;
};

} // namespace hft
