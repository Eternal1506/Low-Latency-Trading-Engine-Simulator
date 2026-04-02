# LXTS — Low-Latency Trading Engine Simulator

A high-performance trading engine simulator demonstrating **low-level systems programming**, **multithreading**, **networking**, and **real-time market data processing** — core skills for HFT / SWE roles.

---

## Architecture

```
Market Data Feed (UDP multicast 239.1.1.1:9900)
        │
        ▼
  FeedSubscriber          ← parses BOOK / TRADE messages
        │  on_book()
        ▼
    Strategy              ← MomentumStrategy | MarketMaker | MeanReversion
        │  om.submit()
        ▼
  OrderManager            ← non-blocking submit, threaded fill simulation
        │  fill_callback
        ▼
  RedisStateManager       ← writes price / position / PnL / fill log
        │
        ▼
  Python Analysis Tools   ← dashboard, PnL chart, latency histogram
```

---

## Project Structure

```
lxts/
├── engine/
│   ├── engine.hpp          # Core types: OrderBook, Order, Fill, Side, etc.
│   └── order_manager.hpp   # Threaded order submission & fill simulation
├── feed/
│   └── feed.hpp            # UDP multicast publisher + subscriber
├── strategy/
│   └── strategies.hpp      # MarketMaker, Momentum (EMA), MeanReversion (BB)
├── state/
│   └── redis_state.hpp     # hiredis wrapper for state persistence
├── analysis/
│   ├── tools.py            # Python dashboard + charts
│   └── bench_latency.cpp   # Standalone latency benchmark
├── docker/
│   ├── Dockerfile
│   └── docker-compose.yml
├── main.cpp                # Engine entry point
└── CMakeLists.txt
```

---

## Prerequisites

### System packages

```bash
# Ubuntu / Debian
sudo apt install cmake build-essential libhiredis-dev

# macOS
brew install cmake hiredis
```

### Python (for analysis tools)

```bash
pip install redis pandas matplotlib rich
```

### Redis

```bash
# Docker (recommended)
docker run -d -p 6379:6379 redis:7-alpine

# or native
sudo apt install redis-server && redis-server
```

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Produces two binaries:
- `build/lxts_engine`     — main trading engine
- `build/bench_latency`   — latency benchmark

---

## Run

### Engine

```bash
# Default: momentum strategy on AAPL
./build/lxts_engine

# Custom strategy and symbol
./build/lxts_engine --strategy market_make --symbol NVDA
./build/lxts_engine --strategy mean_revert --symbol SPY
```

Available strategies: `momentum` | `market_make` | `mean_revert`
Available symbols:    `AAPL` `SPY` `QQQ` `TSLA` `NVDA`

### With Docker Compose

```bash
cd docker
STRATEGY=momentum SYMBOL=AAPL docker compose up --build
```

---

## Analysis Tools

All tools connect to Redis. Run with the engine active.

```bash
# Live terminal dashboard
python lxts/analysis/tools.py dashboard

# Last 50 fills
python lxts/analysis/tools.py fills

# Animated PnL chart
python lxts/analysis/tools.py pnl --symbol AAPL

# Latency distribution histogram
python lxts/analysis/tools.py latency
```

---

## Latency Benchmark

```bash
./build/bench_latency
```

Sample output:
```
Orders:  10000
Mean:    47.3 us
Median:  45 us
p95:     74 us
p99:     82 us
p99.9:   89 us
Max:     97 us
```

---

## Key Technical Concepts Demonstrated

| Area | Implementation |
|---|---|
| Multithreading | `OrderManager` worker thread, feed subscriber thread, strategy on calling thread |
| Lock-free design | `std::atomic` for stop flags and sequence counters |
| UDP networking | Multicast publish/subscribe for market data feed |
| Memory management | Smart pointers (`shared_ptr`) for order lifecycle, zero dynamic alloc in hot path |
| Concurrency | `std::mutex` guards on order book and price maps |
| Low-level timing | `std::chrono::steady_clock` for microsecond-resolution latency measurement |
| Redis integration | hiredis C client for state persistence and fill logging |
| Docker | Multi-stage build (builder + slim runtime image) |

---

## Extending

### Add a new strategy

1. Subclass `lxts::Strategy` in `strategy/strategies.hpp`
2. Implement `on_book()` and `on_fill()`
3. Add a branch in `main.cpp` CLI parser

### Add a new symbol

Add a row to the `base_price()` / `base_vol()` helpers in `feed/feed.hpp` and to the `SYMBOLS` list in `main.cpp`.

### Connect to a real feed

Replace `FeedPublisher` with a real market data adapter (e.g. IEX TOPS, Alpaca, Interactive Brokers) — the `FeedSubscriber` interface accepts any `BookCallback`.

---
