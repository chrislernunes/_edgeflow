#include "signal_engine.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace hft {

SignalEngine::SignalEngine(Config cfg) : cfg_(std::move(cfg)) {}

void SignalEngine::on_trade(const TradeEvent& ev)
{
    TradeRecord r;
    r.ts_us  = ev.ts_us;
    r.price  = ev.price;
    r.qty    = ev.qty;
    r.is_buy = !ev.is_buyer_maker;
    trades_.push_back(r);
    evict_old_trades(ev.ts_us);
}

void SignalEngine::evict_old_trades(uint64_t now_us)
{
    uint64_t cutoff = (now_us > cfg_.flow_window_us)
                      ? now_us - cfg_.flow_window_us : 0;
    while (!trades_.empty() && trades_.front().ts_us < cutoff)
        trades_.pop_front();
}

SignalFlowStats SignalEngine::current_flow_stats(uint64_t now_us) const
{
    uint64_t cutoff = (now_us > cfg_.flow_window_us)
                      ? now_us - cfg_.flow_window_us : 0;
    SignalFlowStats fs{};
    double bn = 0.0, sn = 0.0;
    for (const auto& t : trades_) {
        if (t.ts_us < cutoff) continue;
        if (t.is_buy) { fs.buy_volume  += t.qty; bn += t.price * t.qty; }
        else          { fs.sell_volume += t.qty; sn += t.price * t.qty; }
        ++fs.count;
    }
    double total   = fs.buy_volume + fs.sell_volume;
    fs.trade_flow  = (total > 0.0) ? (fs.buy_volume - fs.sell_volume) / total : 0.0;
    fs.vwap_buy    = (fs.buy_volume  > 0.0) ? bn / fs.buy_volume  : 0.0;
    fs.vwap_sell   = (fs.sell_volume > 0.0) ? sn / fs.sell_volume : 0.0;
    return fs;
}

SignalSnapshot::SpreadRegime SignalEngine::classify_spread(double spread_bps) const
{
    if (spread_bps <= cfg_.tight_spread_bps) return SignalSnapshot::SpreadRegime::TIGHT;
    if (spread_bps >= cfg_.wide_spread_bps)  return SignalSnapshot::SpreadRegime::WIDE;
    return SignalSnapshot::SpreadRegime::NORMAL;
}

double SignalEngine::compute_composite(const SignalSnapshot& s) const
{
    double mp_signal = 0.0;
    if (s.mid > 0.0) {
        double norm = s.microprice_offset_bps / 2.0;
        mp_signal   = std::max(-1.0, std::min(1.0, norm));
    }
    double flow = s.trade_flow;
    if (cfg_.min_flow_volume > 0.0 &&
        (s.buy_volume + s.sell_volume) < cfg_.min_flow_volume)
        flow = 0.0;

    double raw   = cfg_.w_obi_l1     * s.obi_L1
                 + cfg_.w_obi_l5     * s.obi_L5
                 + cfg_.w_microprice * mp_signal
                 + cfg_.w_trade_flow * flow;
    double w_sum = cfg_.w_obi_l1 + cfg_.w_obi_l5 + cfg_.w_microprice + cfg_.w_trade_flow;
    return std::max(-1.0, std::min(1.0, (w_sum > 0.0) ? raw / w_sum : 0.0));
}

SignalSnapshot SignalEngine::compute(const OrderBook& book,
                                     uint64_t local_ts_us,
                                     uint64_t exchange_ts_ms)
{
    evict_old_trades(local_ts_us);
    SignalSnapshot s{};
    s.ts_us          = local_ts_us;
    s.exchange_ts_ms = exchange_ts_ms;

    auto bst = book.stats();
    if (!bst) return s;

    s.best_bid   = bst->best_bid;
    s.best_ask   = bst->best_ask;
    s.mid        = bst->mid;
    s.spread     = bst->spread;
    s.spread_bps = bst->spread_bps;

    auto mp  = book.microprice();
    s.microprice            = mp.value_or(s.mid);
    s.microprice_offset_bps = (s.mid > 0.0)
        ? (s.microprice - s.mid) / s.mid * 10000.0 : 0.0;

    auto obi = [&](int n) { return book.obi(n).value_or(0.0); };
    s.obi_L1  = obi(1);
    s.obi_L3  = obi(3);
    s.obi_L5  = obi(5);
    s.obi_L10 = obi(10);

    auto fs       = current_flow_stats(local_ts_us);
    s.buy_volume  = fs.buy_volume;
    s.sell_volume = fs.sell_volume;
    s.trade_flow  = fs.trade_flow;
    s.vwap_buy    = fs.vwap_buy;
    s.vwap_sell   = fs.vwap_sell;
    s.trade_count = fs.count;

    s.spread_regime = classify_spread(s.spread_bps);
    s.composite     = compute_composite(s);
    return s;
}

std::string SignalSnapshot::to_string() const
{
    std::ostringstream ss;
    ss << std::fixed
       << "[" << exchange_ts_ms << "]"
       << "  mid="  << std::setprecision(2) << mid
       << "  sprd=" << std::setprecision(4) << spread_bps << "bps"
       << "  obi="  << std::showpos << std::setprecision(3) << obi_L1  << std::noshowpos
       << "  mp="   << std::showpos << std::setprecision(4) << microprice_offset_bps << std::noshowpos << "bps"
       << "  flow=" << std::showpos << std::setprecision(3) << trade_flow << std::noshowpos
       << "  sig="  << std::showpos << std::setprecision(3) << composite  << std::noshowpos
       << "  [" << spread_regime_str() << "]\n";
    return ss.str();
}

} // namespace hft
