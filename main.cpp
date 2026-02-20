#include "binance_depth_feed.h"
#include "signal_engine.h"
#include "signal_validator.h"
#include "execution_simulator.h"
#include <iostream>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_stop{false};
static void sigint_handler(int) { g_stop.store(true); }

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::signal(SIGINT, sigint_handler);

    std::string symbol = (argc > 1) ? argv[1] : "BTCUSDT";

    // ── Phase 2: Signal Engine ────────────────────────────────────────────────
    hft::SignalEngine::Config sig_cfg;
    sig_cfg.flow_window_us   = 500000u;
    sig_cfg.tight_spread_bps = 1.0;
    sig_cfg.wide_spread_bps  = 5.0;
    sig_cfg.w_obi_l1         = 0.30;
    sig_cfg.w_obi_l5         = 0.20;
    sig_cfg.w_microprice     = 0.20;
    sig_cfg.w_trade_flow     = 0.30;
    sig_cfg.min_flow_volume  = 0.0;
    hft::SignalEngine engine(sig_cfg);

    // ── Phase 3: Multi-Horizon Validator ──────────────────────────────────────
    hft::MultiHorizonValidator::Config val_cfg;
    val_cfg.horizons_us[0]    = 50000u;
    val_cfg.horizons_us[1]    = 100000u;
    val_cfg.horizons_us[2]    = 250000u;
    val_cfg.horizons_us[3]    = 500000u;
    val_cfg.min_composite_abs = 0.05;
    val_cfg.max_pending       = 20000;
    hft::MultiHorizonValidator validator(val_cfg);

    // ── Phase 4: Execution Simulator ─────────────────────────────────────────
    hft::ExecutionSimulator::Config exec_cfg;
    exec_cfg.passive_entry   = false;
    exec_cfg.min_signal      = 0.20;
    exec_cfg.hold_us         = 250000u;
    exec_cfg.fill_timeout_us = 100000u;
    exec_cfg.taker_fee_bps   = 0.5;
    exec_cfg.maker_fee_bps   = 0.2;
    exec_cfg.max_spread_bps  = 3.0;
    exec_cfg.max_inventory   = 1;
    hft::ExecutionSimulator sim(exec_cfg);

    auto last_report = std::chrono::steady_clock::now();
    constexpr int REPORT_INTERVAL_S = 30;

    // ── Restart loop (handles gaps without dying) ─────────────────────────────
    while (!g_stop.load()) {

        hft::BinanceDepthFeed::Config ws_cfg;
        ws_cfg.symbol  = symbol;
        ws_cfg.verbose = false;
        hft::BinanceDepthFeed feed(ws_cfg);

        // ── Wire aggTrade → SignalEngine ──────────────────────────────────────
        feed.on_trade_event([&](const hft::TradeEvent& tev) {
            engine.on_trade(tev);
        });

        // ── Wire depth → pipeline ─────────────────────────────────────────────
        feed.on_book_update([&](const hft::OrderBook& book,
                                 const hft::DepthEvent& ev)
        {
            // Phase 2
            auto snap = engine.compute(book, ev.local_ts_us, ev.event_ts_ms);

            // Phase 3
            validator.on_signal(snap);
            validator.update_mid(ev.local_ts_us, snap.mid);

            // Phase 4
            sim.on_signal(snap);
            sim.update(ev.local_ts_us, snap.mid, snap.best_bid, snap.best_ask);

            // Stream
            std::cout << snap.to_string();

            // Periodic report
            auto now = std::chrono::steady_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_report).count();
            if (secs >= REPORT_INTERVAL_S) {
                last_report = now;
                validator.print_report();
                sim.print_report();
            }
        });

        feed.on_error([](const std::string&) {});

        try {
            feed.start();
            std::cout << "Synced. Streaming " << symbol
                      << "  [report every " << REPORT_INTERVAL_S
                      << "s]  Ctrl-C for final report\n";

            while (!g_stop.load() && feed.is_running())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

        } catch (const std::exception& e) {
            std::cerr << "[WARN] " << e.what() << " retrying...\n";
        }

        feed.stop();

        if (!g_stop.load()) {
            std::cerr << "[INFO] Re-syncing in 2s...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // ── Final report ──────────────────────────────────────────────────────────
    std::cout << "\n--- FINAL REPORT ---";
    validator.print_report();
    sim.print_report();
    std::cout << "Shutdown.\n";
    return 0;
}