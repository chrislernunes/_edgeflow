#include "order_book.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hft {

OrderBook::OrderBook(std::string symbol, int depth_levels)
    : symbol_(std::move(symbol)), depth_levels_(depth_levels) {}

void OrderBook::apply_side(std::map<double, double, std::greater<double>>& book,
                            const std::vector<Level>& levels)
{
    for (const auto& l : levels) {
        if (l.qty == 0.0) book.erase(l.price);
        else              book[l.price] = l.qty;
    }
}

void OrderBook::apply_side(std::map<double, double>& book,
                            const std::vector<Level>& levels)
{
    for (const auto& l : levels) {
        if (l.qty == 0.0) book.erase(l.price);
        else              book[l.price] = l.qty;
    }
}

void OrderBook::apply_snapshot(uint64_t last_update_id,
                                const std::vector<Level>& bids,
                                const std::vector<Level>& asks)
{
    bids_.clear();
    asks_.clear();
    for (const auto& l : bids) if (l.qty > 0.0) bids_[l.price] = l.qty;
    for (const auto& l : asks) if (l.qty > 0.0) asks_[l.price] = l.qty;
    last_update_id_   = last_update_id;
    last_event_ts_ms_ = 0;
    synced_           = true;
    gap_count_        = 0;
}

bool OrderBook::apply_update(uint64_t U, uint64_t u, uint64_t pu,
                              const std::vector<Level>& bids,
                              const std::vector<Level>& asks,
                              uint64_t event_ts_ms,
                              uint64_t local_ts_us)
{
    if (!synced_) return false;
    if (u <= last_update_id_) return true; // stale, skip silently

    if (last_event_ts_ms_ == 0) {
        // First event post-snapshot
        if (!(U <= last_update_id_ + 1 && last_update_id_ + 1 <= u)) {
            ++gap_count_; synced_ = false; return false;
        }
    } else {
        if (pu != last_update_id_) {
            ++gap_count_; synced_ = false; return false;
        }
    }

    apply_side(bids_, bids);
    apply_side(asks_, asks);
    last_update_id_   = u;
    last_event_ts_ms_ = event_ts_ms;
    last_local_ts_us_ = local_ts_us;
    return true;
}

void OrderBook::reset()
{
    bids_.clear();
    asks_.clear();
    synced_           = false;
    last_update_id_   = 0;
    last_event_ts_ms_ = 0;
    last_local_ts_us_ = 0;
}

std::optional<Level> OrderBook::best_bid() const
{
    if (bids_.empty()) return std::nullopt;
    auto it = bids_.begin();
    return Level{it->first, it->second};
}

std::optional<Level> OrderBook::best_ask() const
{
    if (asks_.empty()) return std::nullopt;
    auto it = asks_.begin();
    return Level{it->first, it->second};
}

std::optional<BookStats> OrderBook::stats() const
{
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb || !ba) return std::nullopt;
    BookStats s;
    s.best_bid   = bb->price;
    s.best_ask   = ba->price;
    s.mid        = (s.best_bid + s.best_ask) * 0.5;
    s.spread     = s.best_ask - s.best_bid;
    s.spread_bps = (s.mid > 0.0) ? s.spread / s.mid * 10000.0 : 0.0;
    return s;
}

std::optional<double> OrderBook::microprice() const
{
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb || !ba) return std::nullopt;
    double total = bb->qty + ba->qty;
    if (total == 0.0) return std::nullopt;
    double w = ba->qty / total;
    return bb->price * w + ba->price * (1.0 - w);
}

std::optional<double> OrderBook::obi(int levels) const
{
    double bq = 0.0, aq = 0.0;
    int c = 0;
    for (auto& [p, q] : bids_) { if (c++ >= levels) break; bq += q; }
    c = 0;
    for (auto& [p, q] : asks_) { if (c++ >= levels) break; aq += q; }
    double total = bq + aq;
    if (total == 0.0) return std::nullopt;
    return (bq - aq) / total;
}

std::vector<Level> OrderBook::bids(int n) const
{
    std::vector<Level> out;
    int c = 0;
    for (auto& [p, q] : bids_) { if (c++ >= n) break; out.push_back({p, q}); }
    return out;
}

std::vector<Level> OrderBook::asks(int n) const
{
    std::vector<Level> out;
    int c = 0;
    for (auto& [p, q] : asks_) { if (c++ >= n) break; out.push_back({p, q}); }
    return out;
}

double OrderBook::bid_depth_bps(double bps) const
{
    auto s = stats();
    if (!s) return 0.0;
    double cutoff = s->mid * (1.0 - bps / 10000.0);
    double total  = 0.0;
    for (auto& [p, q] : bids_) { if (p < cutoff) break; total += q; }
    return total;
}

double OrderBook::ask_depth_bps(double bps) const
{
    auto s = stats();
    if (!s) return 0.0;
    double cutoff = s->mid * (1.0 + bps / 10000.0);
    double total  = 0.0;
    for (auto& [p, q] : asks_) { if (p > cutoff) break; total += q; }
    return total;
}

std::string OrderBook::to_string(int levels) const
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    ss << "=== " << symbol_ << " uid=" << last_update_id_ << " ===\n";
    auto al = asks(levels);
    for (int i = (int)al.size()-1; i >= 0; --i)
        ss << "  ASK " << std::setw(12) << al[i].price << " | " << al[i].qty << "\n";
    ss << "  ---\n";
    for (auto& l : bids(levels))
        ss << "  BID " << std::setw(12) << l.price << " | " << l.qty << "\n";
    return ss.str();
}

} // namespace hft
