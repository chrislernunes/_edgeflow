#include "binance_depth_feed.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <cctype>
#include <libwebsockets.h>
#include <curl/curl.h>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// CURL write callback
// ─────────────────────────────────────────────────────────────────────────────

static size_t curl_write(void* ptr, size_t size, size_t nmemb, std::string* out)
{
    out->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

// ─────────────────────────────────────────────────────────────────────────────
// libwebsockets
// ─────────────────────────────────────────────────────────────────────────────

static BinanceDepthFeed* g_feed = nullptr;

struct LwsUserData { std::string rx_buf; };

static int lws_cb(struct lws* wsi, enum lws_callback_reasons reason,
                   void* user, void* in, size_t len)
{
    auto* ud = reinterpret_cast<LwsUserData*>(user);
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (g_feed) g_feed->on_ws_connected();
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        ud->rx_buf.append((char*)in, len);
        if (lws_is_final_fragment(wsi)) {
            if (g_feed) g_feed->on_ws_message_public(ud->rx_buf);
            ud->rx_buf.clear();
        }
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_CLOSED:
        if (g_feed) g_feed->signal_reconnect();
        break;
    default: break;
    }
    return 0;
}

static const lws_protocols protocols[] = {
    {"combined", lws_cb, sizeof(LwsUserData), 131072, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

// ─────────────────────────────────────────────────────────────────────────────
// Bridge methods
// ─────────────────────────────────────────────────────────────────────────────

void BinanceDepthFeed::on_ws_connected()                            { ws_connected_.store(true); }
void BinanceDepthFeed::on_ws_message_public(const std::string& msg) { on_ws_message(msg); }
void BinanceDepthFeed::signal_reconnect()                           { need_reconnect_.store(true); }

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

BinanceDepthFeed::BinanceDepthFeed(Config cfg)
    : cfg_(std::move(cfg)), book_(cfg_.symbol, cfg_.depth_levels)
{
    buffer_.reserve(cfg_.buffer_max);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

BinanceDepthFeed::~BinanceDepthFeed()
{
    stop();
    curl_global_cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void BinanceDepthFeed::start()
{
    if (running_.load()) return;
    running_.store(true);
    g_feed     = this;
    ws_thread_ = std::thread(&BinanceDepthFeed::ws_thread_fn, this);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!synced_.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!synced_.load())
        throw std::runtime_error("BinanceDepthFeed: sync timeout");
}

void BinanceDepthFeed::stop()
{
    running_.store(false);
    if (ws_thread_.joinable()) ws_thread_.join();
    g_feed = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// WS thread — combined stream: depth + aggTrade
// ─────────────────────────────────────────────────────────────────────────────

void BinanceDepthFeed::ws_thread_fn()
{
    std::string sym = cfg_.symbol;
    for (auto& c : sym) c = (char)std::tolower((unsigned char)c);

    // Combined stream path: /stream?streams=<sym>@depth@100ms/<sym>@aggTrade
    std::string path = "/stream?streams=" + sym + "@depth@100ms/" + sym + "@aggTrade";

    lws_context_creation_info info{};
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    lws_context* ctx = lws_create_context(&info);
    if (!ctx) throw std::runtime_error("lws_create_context failed");

    while (running_.load()) {
        need_reconnect_.store(false);
        ws_connected_.store(false);
        synced_.store(false);
        snapshot_fetched_ = false;
        book_.reset();
        buffer_.clear();

        lws_client_connect_info ci{};
        ci.context        = ctx;
        ci.address        = cfg_.ws_host.c_str();
        ci.port           = std::stoi(cfg_.ws_port);
        ci.path           = path.c_str();
        ci.host           = cfg_.ws_host.c_str();
        ci.origin         = cfg_.ws_host.c_str();
        ci.ssl_connection = LCCSCF_USE_SSL;
        ci.protocol       = protocols[0].name;

        if (!lws_client_connect_via_info(&ci)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        while (running_.load() && !need_reconnect_.load()) {
            lws_service(ctx, 10);
            do_sync_if_needed();
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    lws_context_destroy(ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// do_sync_if_needed — Binance diff-depth sync protocol
// ─────────────────────────────────────────────────────────────────────────────

void BinanceDepthFeed::do_sync_if_needed()
{
    if (snapshot_fetched_ || !ws_connected_.load() || buffer_.size() < 5) return;

    try {
        auto snap = fetch_snapshot();
        book_.apply_snapshot(snap.last_update_id, snap.bids, snap.asks);

        for (auto& ev : buffer_) {
            if (ev.u <= snap.last_update_id) continue;
            bool ok = book_.apply_update(ev.U, ev.u, ev.pu,
                                          ev.bids, ev.asks,
                                          ev.event_ts_ms, ev.local_ts_us);
            if (!ok) { book_.reset(); buffer_.clear(); return; }
            if (book_cb_) book_cb_(book_, ev);
        }
        buffer_.clear();

        snapshot_fetched_ = true;
        synced_.store(true);
        if (cfg_.verbose)
            std::cout << "[SYNC] uid=" << book_.last_update_id() << "\n";

    } catch (const std::exception& e) {
        std::cerr << "[SYNC] " << e.what() << " retrying...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WS message handler — routes combined stream messages by type
// ─────────────────────────────────────────────────────────────────────────────

void BinanceDepthFeed::on_ws_message(const std::string& msg)
{
    try {
        auto j = nlohmann::json::parse(msg);

        // Combined stream wraps messages as: {"stream":"...","data":{...}}
        // Individual stream (legacy) sends data directly.
        const nlohmann::json* data = &j;
        std::string stream_name;
        if (j.contains("stream") && j.contains("data")) {
            stream_name = j["stream"].get<std::string>();
            data        = &j["data"];
        }

        // Determine message type by event type field "e"
        std::string etype;
        if (data->contains("e")) etype = (*data)["e"].get<std::string>();

        if (etype == "aggTrade") {
            // ── Trade message ─────────────────────────────────────────────────
            if (trade_cb_) {
                auto tev = parse_trade_event(*data);
                trade_cb_(tev);
            }
            return;
        }

        // ── Depth update message ──────────────────────────────────────────────
        // depthUpdate event or no "e" field (legacy depth stream)
        auto ev = parse_depth_event(*data);

        if (!synced_.load()) {
            if ((int)buffer_.size() < cfg_.buffer_max)
                buffer_.push_back(ev);
            return;
        }

        bool ok = book_.apply_update(ev.U, ev.u, ev.pu,
                                      ev.bids, ev.asks,
                                      ev.event_ts_ms, ev.local_ts_us);
        if (!ok) {
            synced_.store(false);
            snapshot_fetched_ = false;
            book_.reset();
            buffer_.clear();
            buffer_.push_back(ev);
            if (error_cb_) error_cb_("sequence gap");
            return;
        }

        if (book_cb_) book_cb_(book_, ev);

    } catch (...) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// REST snapshot
// ─────────────────────────────────────────────────────────────────────────────

SnapshotData BinanceDepthFeed::fetch_snapshot()
{
    std::string url  = "https://" + cfg_.rest_host
                     + "/api/v3/depth?symbol=" + cfg_.symbol + "&limit=1000";
    std::string body;

    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init");
    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    CURLcode res = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK)
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(res));
    if (http_code != 200)
        throw std::runtime_error("HTTP " + std::to_string(http_code));

    auto j = nlohmann::json::parse(body);
    SnapshotData snap;
    snap.last_update_id = j["lastUpdateId"].get<uint64_t>();
    snap.bids           = parse_levels(j["bids"]);
    snap.asks           = parse_levels(j["asks"]);
    return snap;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────────────

DepthEvent BinanceDepthFeed::parse_depth_event(const nlohmann::json& j)
{
    DepthEvent ev;
    ev.local_ts_us = now_us();
    ev.event_ts_ms = j.value("T", j.value("E", uint64_t(0)));
    ev.U           = j["U"].get<uint64_t>();
    ev.u           = j["u"].get<uint64_t>();
    ev.pu          = j.value("pu", uint64_t(0));
    ev.bids        = parse_levels(j["b"]);
    ev.asks        = parse_levels(j["a"]);
    return ev;
}

TradeEvent BinanceDepthFeed::parse_trade_event(const nlohmann::json& j)
{
    // aggTrade fields:
    // "T" = trade time ms, "p" = price, "q" = qty, "m" = is_buyer_maker
    TradeEvent ev;
    ev.ts_us          = now_us();
    ev.price          = std::stod(j["p"].get<std::string>());
    ev.qty            = std::stod(j["q"].get<std::string>());
    ev.is_buyer_maker = j["m"].get<bool>();
    return ev;
}

std::vector<Level> BinanceDepthFeed::parse_levels(const nlohmann::json& arr)
{
    std::vector<Level> out;
    out.reserve(arr.size());
    for (const auto& e : arr)
        out.push_back({std::stod(e[0].get<std::string>()),
                       std::stod(e[1].get<std::string>())});
    return out;
}

uint64_t BinanceDepthFeed::now_us()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace hft