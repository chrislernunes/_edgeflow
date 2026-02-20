#include "signal_validator.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <algorithm>

namespace hft {

MultiHorizonValidator::MultiHorizonValidator(Config cfg) : cfg_(cfg) {}

void MultiHorizonValidator::on_signal(const SignalSnapshot& snap)
{
    if (std::abs(snap.composite) < cfg_.min_composite_abs) return;
    if ((int)pending_.size() >= cfg_.max_pending) pending_.pop_front();

    PendingSignal p;
    p.ts_us          = snap.ts_us;
    p.mid_at_signal  = snap.mid;
    p.composite      = snap.composite;
    p.bucket         = classify_bucket(snap.composite);
    p.regime         = snap.spread_regime;
    p.evaluated_mask = 0;
    pending_.push_back(p);
}

void MultiHorizonValidator::update_mid(uint64_t now_us, double current_mid)
{
    evaluate_pending(now_us, current_mid);
}

void MultiHorizonValidator::evaluate_pending(uint64_t now_us, double current_mid)
{
    uint8_t full_mask = (uint8_t)((1 << N_HORIZONS) - 1);

    for (auto& p : pending_) {
        uint64_t elapsed = now_us - p.ts_us;

        for (int h = 0; h < N_HORIZONS; ++h) {
            uint8_t mask = (uint8_t)(1 << h);
            if (p.evaluated_mask & mask) continue;
            if (elapsed < cfg_.horizons_us[h]) continue;

            double move_bps = (p.mid_at_signal > 0.0)
                ? (current_mid - p.mid_at_signal) / p.mid_at_signal * 10000.0
                : 0.0;

            record(h, p, move_bps);
            p.evaluated_mask |= mask;
            ++total_evaluated_;
        }
    }

    while (!pending_.empty() && pending_.front().evaluated_mask == full_mask)
        pending_.pop_front();
}

void MultiHorizonValidator::record(int h, const PendingSignal& p, double move_bps)
{
    bool correct = (p.composite > 0.0 && move_bps > 0.0) ||
                   (p.composite < 0.0 && move_bps < 0.0);

    auto& bs = stats_[h][(int)p.bucket];
    bs.total++;
    bs.sum_move_bps += std::abs(move_bps);
    bs.sum_move_sq  += move_bps * move_bps;
    if (correct) bs.correct++;

    auto& rs = regime_stats_[h][regime_index(p.regime)];
    rs.total++;
    rs.sum_move_bps += std::abs(move_bps);
    rs.sum_move_sq  += move_bps * move_bps;
    if (correct) rs.correct++;
}

int MultiHorizonValidator::regime_index(SignalSnapshot::SpreadRegime r)
{
    switch (r) {
    case SignalSnapshot::SpreadRegime::TIGHT:  return 0;
    case SignalSnapshot::SpreadRegime::NORMAL: return 1;
    case SignalSnapshot::SpreadRegime::WIDE:   return 2;
    }
    return 1;
}

std::string MultiHorizonValidator::report_string() const
{
    const char* hl[N_HORIZONS]  = {"50ms ", "100ms", "250ms", "500ms"};
    const char* rl[N_REGIMES]   = {"TIGHT ", "NORMAL", "WIDE  "};

    std::ostringstream ss;
    ss << std::fixed;
    ss << "\n=================================================================\n";
    ss << "  EDGEFLOW — Signal Validator Report\n";
    ss << "=================================================================\n";
    ss << "  Observations evaluated : " << total_evaluated_ << "\n";
    ss << "  Pending                : " << pending_.size()  << "\n\n";

    ss << "-----------------------------------------------------------------\n";
    ss << "  BY SIGNAL BUCKET\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Bucket      Horizon  N        HitRate   AvgMove(bps)  Sharpe\n";
    for (int b = 0; b < N_BUCKETS; ++b) {
        for (int h = 0; h < N_HORIZONS; ++h) {
            const auto& s = stats_[h][b];
            if (s.total == 0) continue;
            ss << "  " << bucket_name((SignalBucket)b)
               << "  " << hl[h]
               << "  " << std::setw(7) << s.total
               << "  " << std::setw(6) << std::setprecision(1) << s.hit_rate()*100.0 << "%"
               << "   " << std::setw(10)<< std::setprecision(4) << s.avg_move_bps()
               << "   " << std::setw(6) << std::setprecision(3) << s.sharpe()
               << "\n";
        }
        ss << "\n";
    }

    ss << "-----------------------------------------------------------------\n";
    ss << "  BY SPREAD REGIME\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Regime    Horizon  N        HitRate   AvgMove(bps)\n";
    for (int r = 0; r < N_REGIMES; ++r) {
        for (int h = 0; h < N_HORIZONS; ++h) {
            const auto& s = regime_stats_[h][r];
            if (s.total == 0) continue;
            ss << "  " << rl[r]
               << "  " << hl[h]
               << "  " << std::setw(7) << s.total
               << "  " << std::setw(6) << std::setprecision(1) << s.hit_rate()*100.0 << "%"
               << "   " << std::setw(10)<< std::setprecision(4) << s.avg_move_bps()
               << "\n";
        }
        ss << "\n";
    }

    ss << "-----------------------------------------------------------------\n";
    ss << "  ALPHA DECAY CURVE (all buckets)\n";
    ss << "-----------------------------------------------------------------\n";
    ss << "  Horizon  N        HitRate   AvgMove(bps)  Sharpe\n";
    for (int h = 0; h < N_HORIZONS; ++h) {
        HorizonStats agg{};
        for (int b = 0; b < N_BUCKETS; ++b) {
            const auto& s = stats_[h][b];
            agg.total        += s.total;
            agg.correct      += s.correct;
            agg.sum_move_bps += s.sum_move_bps;
            agg.sum_move_sq  += s.sum_move_sq;
        }
        if (agg.total == 0) continue;
        ss << "  " << hl[h]
           << "  " << std::setw(7) << agg.total
           << "  " << std::setw(6) << std::setprecision(1) << agg.hit_rate()*100.0 << "%"
           << "   " << std::setw(10)<< std::setprecision(4) << agg.avg_move_bps()
           << "   " << std::setw(6) << std::setprecision(3) << agg.sharpe()
           << "\n";
    }
    ss << "=================================================================\n";
    return ss.str();
}

void MultiHorizonValidator::print_report() const
{
    std::cout << report_string() << std::flush;
}

} // namespace hft
