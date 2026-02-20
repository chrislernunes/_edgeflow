#pragma once
#include "signal_engine.h"
#include <cstdint>
#include <deque>
#include <string>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// SignalBucket — |composite| strength classification
// ─────────────────────────────────────────────────────────────────────────────

enum class SignalBucket { WEAK = 0, MODERATE, STRONG, VERY_STRONG, COUNT };

inline SignalBucket classify_bucket(double composite)
{
    double a = composite < 0.0 ? -composite : composite;
    if (a < 0.2) return SignalBucket::WEAK;
    if (a < 0.5) return SignalBucket::MODERATE;
    if (a < 0.8) return SignalBucket::STRONG;
    return SignalBucket::VERY_STRONG;
}

inline const char* bucket_name(SignalBucket b)
{
    switch (b) {
    case SignalBucket::WEAK:        return "|sig|<0.2";
    case SignalBucket::MODERATE:    return "0.2-0.5  ";
    case SignalBucket::STRONG:      return "0.5-0.8  ";
    case SignalBucket::VERY_STRONG: return "|sig|>0.8";
    default:                        return "?        ";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HorizonStats
// ─────────────────────────────────────────────────────────────────────────────

struct HorizonStats {
    uint64_t total        = 0;
    uint64_t correct      = 0;
    double   sum_move_bps = 0.0;
    double   sum_move_sq  = 0.0;

    double hit_rate()     const { return total > 0 ? (double)correct / total : 0.0; }
    double avg_move_bps() const { return total > 0 ? sum_move_bps / total    : 0.0; }
    double sharpe()       const {
        if (total < 2) return 0.0;
        double mean = avg_move_bps();
        double var  = sum_move_sq / total - mean * mean;
        return var > 0.0 ? mean / __builtin_sqrt(var) : 0.0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// PendingSignal
// ─────────────────────────────────────────────────────────────────────────────

struct PendingSignal {
    uint64_t     ts_us         = 0;
    double       mid_at_signal = 0.0;
    double       composite     = 0.0;
    SignalBucket bucket        = SignalBucket::WEAK;
    SignalSnapshot::SpreadRegime regime = SignalSnapshot::SpreadRegime::NORMAL;
    uint8_t      evaluated_mask = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MultiHorizonValidatorConfig — hoisted outside class (MinGW fix)
// ─────────────────────────────────────────────────────────────────────────────

struct MultiHorizonValidatorConfig {
    uint64_t horizons_us[4]    = {50000u, 100000u, 250000u, 500000u};
    double   min_composite_abs = 0.05;
    int      max_pending       = 20000;
};

// ─────────────────────────────────────────────────────────────────────────────
// MultiHorizonValidator
// ─────────────────────────────────────────────────────────────────────────────

class MultiHorizonValidator {
public:
    static constexpr int N_HORIZONS = 4;
    static constexpr int N_BUCKETS  = (int)SignalBucket::COUNT;
    static constexpr int N_REGIMES  = 3;

    using Config = MultiHorizonValidatorConfig;

    explicit MultiHorizonValidator(Config cfg = Config{});

    void on_signal(const SignalSnapshot& snap);
    void update_mid(uint64_t now_us, double current_mid);

    std::string report_string() const;
    void        print_report()  const;

    size_t   pending_count()   const { return pending_.size(); }
    uint64_t total_evaluated() const { return total_evaluated_; }

private:
    Config cfg_;

    std::deque<PendingSignal> pending_;
    HorizonStats stats_[N_HORIZONS][N_BUCKETS];
    HorizonStats regime_stats_[N_HORIZONS][N_REGIMES];
    uint64_t     total_evaluated_ = 0;

    void evaluate_pending(uint64_t now_us, double current_mid);
    void record(int h, const PendingSignal& p, double move_bps);
    static int regime_index(SignalSnapshot::SpreadRegime r);
};

} // namespace hft
