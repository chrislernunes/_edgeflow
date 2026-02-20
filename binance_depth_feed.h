#pragma once
#include "order_book.h"
#include "signal_engine.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// DepthEvent — one diff-depth WebSocket message
// ─────────────────────────────────────────────────────────────────────────────

struct DepthEvent {
    uint64_t           local_ts_us  = 0;
    uint64_t           event_ts_ms  = 0;
    uint64_t           U            = 0;   // first update ID
    uint64_t           u            = 0;   // last  update ID
    uint64_t           pu           = 0;   // prev last update ID
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// ─────────────────────────────────────────────────────────────────────────────
// SnapshotData — REST /api/v3/depth response
// ─────────────────────────────────────────────────────────────────────────────

struct SnapshotData {
    uint64_t           last_update_id = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// ─────────────────────────────────────────────────────────────────────────────
// BinanceDepthFeedConfig — hoisted outside class (MinGW fix)
// ─────────────────────────────────────────────────────────────────────────────

struct BinanceDepthFeedConfig {
    std::string symbol       = "BTCUSDT";
    std::string ws_host      = "stream.binance.com";
    std::string ws_port      = "9443";
    std::string rest_host    = "api.binance.com";
    int         depth_levels = 20;
    int         buffer_max   = 200;
    bool        verbose      = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// BinanceDepthFeed
// ─────────────────────────────────────────────────────────────────────────────

class BinanceDepthFeed {
public:
    using Config        = BinanceDepthFeedConfig;
    using BookCallback  = std::function<void(const OrderBook&, const DepthEvent&)>;
    using TradeCallback = std::function<void(const TradeEvent&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit BinanceDepthFeed(Config cfg = Config{});
    ~BinanceDepthFeed();

    void on_book_update(BookCallback  cb) { book_cb_  = std::move(cb); }
    void on_trade_event(TradeCallback cb) { trade_cb_ = std::move(cb); }
    void on_error      (ErrorCallback cb) { error_cb_ = std::move(cb); }

    void start();
    void stop();

    bool     is_running() const { return running_.load(); }
    bool     is_synced()  const { return synced_.load();  }

    // Internal — called from static lws callback
    void on_ws_connected();
    void on_ws_message_public(const std::string& msg);
    void signal_reconnect();

private:
    Config       cfg_;
    OrderBook    book_;
    BookCallback  book_cb_;
    TradeCallback trade_cb_;
    ErrorCallback error_cb_;

    std::atomic<bool> running_       {false};
    std::atomic<bool> synced_        {false};
    std::atomic<bool> ws_connected_  {false};
    std::atomic<bool> need_reconnect_{false};

    std::thread ws_thread_;

    // Pre-snapshot buffer
    std::vector<DepthEvent> buffer_;
    bool snapshot_fetched_ = false;

    void ws_thread_fn();
    void do_sync_if_needed();
    void on_ws_message(const std::string& msg);

    SnapshotData        fetch_snapshot();
    DepthEvent          parse_depth_event(const nlohmann::json& j);
    TradeEvent          parse_trade_event(const nlohmann::json& j);
    std::vector<Level>  parse_levels     (const nlohmann::json& arr);

    static uint64_t now_us();
};

} // namespace hft