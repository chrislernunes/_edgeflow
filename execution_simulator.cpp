#include "execution_simulator.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <algorithm>

namespace hft {

ExecutionSimulator::ExecutionSimulator(Config cfg) : cfg_(std::move(cfg)) {}

void ExecutionSimulator::on_signal(const SignalSnapshot& snap)
{
    if (!can_enter())                                return;
    if (snap.spread_bps > cfg_.max_spread_bps)       return;
    if (std::abs(snap.composite) < cfg_.min_signal)  return;

    bool is_long = snap.composite > 0.0;

    OpenPosition pos;
    pos.entry_ts_us      = snap.ts_us;
    pos.fill_deadline_us = snap.ts_us + cfg_.fill_timeout_us;
    pos.signal_value     = snap.composite;
    pos.spread_bps       = snap.spread_bps;
    pos.is_long          = is_long;
    pos.passive          = cfg_.passive_entry;

    if (cfg_.passive_entry) {
        pos.entry_price = is_long ? snap.best_bid : snap.best_ask;
        pos.filled      = false;
    } else {
        pos.entry_price = is_long ? snap.best_ask : snap.best_bid;
        pos.filled      = true;
    }

    open_.push_back(pos);
}

void ExecutionSimulator::update(uint64_t now_us, double mid,
                                 double best_bid, double best_ask)
{
    for (auto it = open_.begin(); it != open_.end(); ) {
        auto& pos = *it;

        if (!pos.filled) {
            bool fill = (pos.is_long  && best_ask <= pos.entry_price) ||
                        (!pos.is_long && best_bid >= pos.entry_price);
            if (!fill && now_us >= pos.fill_deadline_us) {
                ++n_expired_;
                it = open_.erase(it);
                continue;
            }
            if (fill) pos.filled = true;
        }

        if (pos.filled && (now_us - pos.entry_ts_us) >= cfg_.hold_us) {
            double exit_price = mid;
            double move       = exit_price - pos.entry_price;
            if (!pos.is_long) move = -move;

            double raw_bps = (pos.entry_price > 0.0)
                ? move / pos.entry_price * 10000.0 : 0.0;
            double net_bps = raw_bps - fee_bps(pos.passive);

            SimTrade t;
            t.entry_ts_us         = pos.entry_ts_us;
            t.exit_ts_us          = now_us;
            t.entry_price         = pos.entry_price;
            t.exit_price          = exit_price;
            t.signal_at_entry     = pos.signal_value;
            t.spread_at_entry_bps = pos.spread_bps;
            t.is_long             = pos.is_long;
            t.pnl_bps             = net_bps;
            trades_.push_back(t);

            total_pnl_bps_ += net_bps;
            ++n_trades_;
            if (net_bps > 0.0) ++n_wins_;

            it = open_.erase(it);
            continue;
        }

        ++it;
    }
}

std::string ExecutionSimulator::report_string() const
{
    std::ostringstream ss;
    ss << std::fixed;
    ss << "\n=================================================================\n";
    ss << "  EDGEFLOW — Execution Simulator Report\n";
    ss << "=================================================================\n";
    ss << "  Entry mode      : " << (cfg_.passive_entry ? "PASSIVE" : "AGGRESSIVE") << "\n";
    ss << "  Signal threshold: |sig| > " << std::setprecision(2) << cfg_.min_signal << "\n";
    ss << "  Hold period     : " << cfg_.hold_us / 1000u << " ms\n";
    ss << "  Fees            : taker=" << cfg_.taker_fee_bps
       << "bps  maker=" << cfg_.maker_fee_bps << "bps\n\n";

    ss << "  Trades completed: " << n_trades_  << "\n";
    ss << "  Expired unfilled: " << n_expired_ << "\n";
    ss << "  Open positions  : " << open_.size() << "\n\n";

    if (n_trades_ == 0) {
        ss << "  No completed trades yet.\n";
        ss << "=================================================================\n";
        return ss.str();
    }

    double avg_pnl  = total_pnl_bps_ / (double)n_trades_;
    double win_rate = (double)n_wins_ / (double)n_trades_ * 100.0;

    double sum_sq = 0.0;
    for (const auto& t : trades_) sum_sq += t.pnl_bps * t.pnl_bps;
    double var    = sum_sq / n_trades_ - avg_pnl * avg_pnl;
    double sharpe = (var > 0.0) ? avg_pnl / std::sqrt(var) : 0.0;

    ss << "-----------------------------------------------------------------\n";
    ss << "  OVERALL\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Total P&L       : " << std::setprecision(3) << total_pnl_bps_ << " bps\n";
    ss << "  Avg P&L / trade : " << std::setprecision(4) << avg_pnl        << " bps\n";
    ss << "  Win rate        : " << std::setprecision(1) << win_rate        << "%\n";
    ss << "  Sharpe (raw)    : " << std::setprecision(3) << sharpe          << "\n\n";

    double   bp[4]  = {};
    uint64_t bn[4]  = {};
    uint64_t bw[4]  = {};
    for (const auto& t : trades_) {
        int b = (int)classify_bucket(t.signal_at_entry);
        bp[b] += t.pnl_bps; ++bn[b];
        if (t.pnl_bps > 0.0) ++bw[b];
    }

    ss << "-----------------------------------------------------------------\n";
    ss << "  BY SIGNAL BUCKET\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Bucket      N       WinRate   AvgPnL(bps)\n";
    for (int b = 0; b < 4; ++b) {
        if (bn[b] == 0) continue;
        ss << "  " << bucket_name((SignalBucket)b)
           << "  " << std::setw(6) << bn[b]
           << "  " << std::setw(6) << std::setprecision(1)
                   << (double)bw[b] / bn[b] * 100.0 << "%"
           << "   " << std::setw(9) << std::setprecision(4)
                   << bp[b] / bn[b] << "\n";
    }

    ss << "\n-----------------------------------------------------------------\n";
    ss << "  LAST 10 TRADES\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Dir    Signal  Entry      Exit       PnL(bps)\n";
    int start = std::max(0, (int)trades_.size() - 10);
    for (int i = start; i < (int)trades_.size(); ++i) {
        const auto& t = trades_[i];
        ss << "  " << (t.is_long ? "LONG " : "SHORT")
           << "  " << std::setw(6) << std::setprecision(3) << t.signal_at_entry
           << "  " << std::setw(9) << std::setprecision(2) << t.entry_price
           << "  " << std::setw(9) << std::setprecision(2) << t.exit_price
           << "  " << std::setw(8) << std::setprecision(4) << t.pnl_bps << "\n";
    }
    ss << "=================================================================\n";
    return ss.str();
}

void ExecutionSimulator::print_report() const
{
    std::cout << report_string() << std::flush;
}

} // namespace hft
