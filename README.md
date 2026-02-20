# edgeflow — HFT Microstructure Engine

A real-time, low-latency market microstructure engine for Binance spot markets. Streams live order book data via WebSocket, computes composite alpha signals from multiple microstructure features, validates signal predictive power across multiple time horizons, and simulates execution with realistic fee modelling.

---

## Architecture

```
Binance WS (depth + aggTrade)
        │
        ▼
   OrderBook          ← maintains synced L2 book (Binance diff-depth protocol)
        │
        ▼
  SignalEngine        ← computes microstructure signals + composite score
        │
   ┌────┴────┐
   ▼         ▼
MultiHorizon  ExecutionSimulator
Validator     ← simulates trades with fees, inventory limits, passive/aggressive entry
   │
   ▼
 Reports (every 30s + on Ctrl-C)
```

---

## Components

### `OrderBook`
Implements the Binance diff-depth sync protocol correctly:
- Buffers WebSocket events before the REST snapshot arrives
- Applies REST snapshot, then replays buffered events
- Validates sequence IDs (`U`, `u`, `pu`) and detects gaps
- Provides L1/L5/L10 OBI, microprice, depth-at-bps, and book stats

### `SignalEngine`
Computes a composite signal `[-1, +1]` from four weighted microstructure features:

| Feature | Weight | Description |
|---|---|---|
| OBI L1 | 0.30 | Order book imbalance at best bid/ask |
| OBI L5 | 0.20 | Order book imbalance across top 5 levels |
| Microprice offset | 0.20 | Qty-weighted mid deviation in bps |
| Trade flow | 0.30 | Buy/sell volume imbalance over rolling window |

Spread regime (TIGHT / NORMAL / WIDE) is classified and attached to every snapshot.

### `MultiHorizonValidator`
Validates signal predictive power without lookahead bias:
- Stores pending signals with their mid price at signal time
- Resolves each signal independently at 50ms, 100ms, 250ms, 500ms horizons
- Reports hit rate, average move (bps), and Sharpe by signal bucket and spread regime
- Alpha decay curve shows how quickly the signal decays

### `ExecutionSimulator`
Simulates round-trip trades with realistic constraints:
- Aggressive (taker) or passive (maker) entry modes
- Passive fill timeout — cancels if not filled within deadline
- Per-trade fee deduction (maker: 0.2 bps/side, taker: 0.5 bps/side)
- Max inventory limit prevents overtrading
- Reports total P&L (bps), win rate, Sharpe, and breakdown by signal bucket

### `BinanceDepthFeed`
WebSocket feed using libwebsockets:
- Combined stream: `@depth@100ms` + `@aggTrade` over a single connection
- Routes depth events to `OrderBook`, trade events to `SignalEngine`
- Auto-reconnects on disconnect or sequence gap

---

## Dependencies

| Library | Purpose |
|---|---|
| libwebsockets | WebSocket client |
| libcurl | REST snapshot fetch |
| nlohmann/json | JSON parsing |
| pthreads | WS thread |
| OpenSSL | TLS |

### Install (MSYS2/UCRT64)
```bash
pacman -S mingw-w64-ucrt-x86_64-libwebsockets
pacman -S mingw-w64-ucrt-x86_64-curl
pacman -S mingw-w64-ucrt-x86_64-nlohmann-json
```

### Install (Ubuntu/Debian)
```bash
sudo apt install libwebsockets-dev libcurl4-openssl-dev nlohmann-json3-dev
```

---

## Build

### Direct (MSYS2/UCRT64 or Linux)
```bash
g++ -std=c++20 -O2 -Iinclude \
    main.cpp order_book.cpp signal_engine.cpp \
    signal_validator.cpp execution_simulator.cpp binance_depth_feed.cpp \
    -o edgeflow -lpthread -lwebsockets -lssl -lcrypto -lcurl
```

### CMake
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

---

## Usage

```bash
./edgeflow BTCUSDT       # default symbol
./edgeflow ETHUSDT
```

Reports print every 30 seconds. Press `Ctrl-C` for a final report and clean shutdown.

---

## Configuration

All config is set in `main.cpp`. Key parameters:

**Signal Engine**
```cpp
sig_cfg.flow_window_us   = 500000u;   // trade flow rolling window (µs)
sig_cfg.tight_spread_bps = 1.0;       // spread threshold for TIGHT regime
sig_cfg.wide_spread_bps  = 5.0;       // spread threshold for WIDE regime
sig_cfg.w_obi_l1         = 0.30;      // OBI L1 weight
sig_cfg.w_obi_l5         = 0.20;      // OBI L5 weight
sig_cfg.w_microprice     = 0.20;      // microprice weight
sig_cfg.w_trade_flow     = 0.30;      // trade flow weight
```

**Validator**
```cpp
val_cfg.horizons_us[0]    = 50000u;   // 50ms
val_cfg.horizons_us[1]    = 100000u;  // 100ms
val_cfg.horizons_us[2]    = 250000u;  // 250ms
val_cfg.horizons_us[3]    = 500000u;  // 500ms
val_cfg.min_composite_abs = 0.05;     // minimum |signal| to validate
```

**Execution Simulator**
```cpp
exec_cfg.passive_entry   = false;     // true = post limit, false = cross spread
exec_cfg.min_signal      = 0.20;      // minimum |composite| to enter
exec_cfg.hold_us         = 250000u;   // hold time before exit (µs)
exec_cfg.fill_timeout_us = 100000u;   // passive fill timeout (µs)
exec_cfg.taker_fee_bps   = 0.5;       // taker fee per side
exec_cfg.maker_fee_bps   = 0.2;       // maker fee per side
exec_cfg.max_spread_bps  = 3.0;       // skip entry if spread exceeds this
exec_cfg.max_inventory   = 1;         // max simultaneous open positions
```

---

## Output Format

**Live stream (every depth update):**
```
[1771554154114]  mid=67204.99  sprd=0.0015bps  obi=+0.286  mp=+0.0002bps  flow=+0.596  sig=+0.143  [TIGHT]
```

**Periodic validator report:**
```
  BY SIGNAL BUCKET
  Bucket      Horizon  N        HitRate   AvgMove(bps)  Sharpe
  0.2-0.5     50ms        45    62.3%       0.0821       0.841
  ...

  ALPHA DECAY CURVE (all buckets)
  Horizon  N        HitRate   AvgMove(bps)  Sharpe
  50ms        82    58.4%       0.0469       0.210
  ...
```

**Periodic simulator report:**
```
  Total P&L       : 12.440 bps
  Avg P&L / trade : +0.478 bps
  Win rate        : 61.5%
  Sharpe (raw)    : 2.134
```

---

## Known Issues / TODOs

- Validator resolves all horizons at the same mid price when they all expire in the same tick — a mid-price history buffer is needed for accurate per-horizon attribution
- `flow=+1.000` saturation occurs when the trade flow window is short relative to trade frequency; increase `flow_window_us` to 5–10 seconds for liquid pairs
- `g_feed` global pointer limits the process to one feed instance; refactor to instance pointer via `lws` user data for multi-symbol support
- Exit is always at mid price; slippage modelling and TWAP exit are not yet implemented
