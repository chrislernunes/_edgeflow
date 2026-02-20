#pragma once
#include <map>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>
#include <functional>

namespace hft {

struct Level {
    double price = 0.0;
    double qty   = 0.0;
};

struct BookStats {
    double best_bid   = 0.0;
    double best_ask   = 0.0;
    double mid        = 0.0;
    double spread     = 0.0;
    double spread_bps = 0.0;
};

class OrderBook {
public:
    explicit OrderBook(std::string symbol = "BTCUSDT", int depth_levels = 20);

    // Snapshot init (REST response)
    void apply_snapshot(uint64_t last_update_id,
                        const std::vector<Level>& bids,
                        const std::vector<Level>& asks);

    // Diff-depth update. Returns false on sequence gap.
    bool apply_update(uint64_t U, uint64_t u, uint64_t pu,
                      const std::vector<Level>& bids,
                      const std::vector<Level>& asks,
                      uint64_t event_ts_ms = 0,
                      uint64_t local_ts_us = 0);

    void reset();

    // Accessors
    bool             is_synced()      const { return synced_; }
    uint64_t         last_update_id() const { return last_update_id_; }
    uint64_t         last_event_ts()  const { return last_event_ts_ms_; }
    uint64_t         last_local_ts()  const { return last_local_ts_us_; }
    uint64_t         gap_count()      const { return gap_count_; }
    const std::string& symbol()       const { return symbol_; }

    std::optional<Level>      best_bid()    const;
    std::optional<Level>      best_ask()    const;
    std::optional<BookStats>  stats()       const;
    std::optional<double>     microprice()  const;
    std::optional<double>     obi(int levels = 1) const;

    std::vector<Level> bids(int n = 5) const;
    std::vector<Level> asks(int n = 5) const;

    double bid_depth_bps(double bps) const;
    double ask_depth_bps(double bps) const;

    std::string to_string(int levels = 5) const;

private:
    void apply_side(std::map<double, double, std::greater<double>>& book,
                    const std::vector<Level>& levels);
    void apply_side(std::map<double, double>& book,
                    const std::vector<Level>& levels);

    std::string symbol_;
    int         depth_levels_;

    std::map<double, double, std::greater<double>> bids_;
    std::map<double, double>                        asks_;

    bool     synced_           = false;
    uint64_t last_update_id_   = 0;
    uint64_t last_event_ts_ms_ = 0;
    uint64_t last_local_ts_us_ = 0;
    uint64_t gap_count_        = 0;
};

} // namespace hft
