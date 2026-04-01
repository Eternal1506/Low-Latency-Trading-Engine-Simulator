#include "engine/engine.hpp"
#include "engine/order_manager.hpp"
#include "feed/feed.hpp"
#include "strategy/strategies.hpp"
#include "state/redis_state.hpp"

#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

// ─────────────────────────────────────────────
// LXTS — Low-Latency Trading Engine Simulator
// Entry point
// ─────────────────────────────────────────────

static volatile bool g_running = true;
void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ── Parse simple CLI args ──────────────────
    std::string strategy_name = "momentum";
    std::string symbol        = "AAPL";
    for (int i = 1; i < argc - 1; i++) {
        if (std::string(argv[i]) == "--strategy") strategy_name = argv[i+1];
        if (std::string(argv[i]) == "--symbol")   symbol        = argv[i+1];
    }

    std::cout << "[LXTS] Starting engine  strategy=" << strategy_name
              << "  symbol=" << symbol << "\n";

    // ── Redis state ────────────────────────────
    lxts::RedisStateManager redis;
    redis.write_status("ACTIVE");

    // ── Order manager ──────────────────────────
    lxts::OrderManager::Config om_cfg;
    om_cfg.min_latency_us = 15;
    om_cfg.max_latency_us = 90;
    auto om = std::make_shared<lxts::OrderManager>(om_cfg);

    // ── Strategy ──────────────────────────────
    std::unique_ptr<lxts::Strategy> strategy;
    if (strategy_name == "market_make") {
        strategy = std::make_unique<lxts::MarketMaker>();
    } else if (strategy_name == "mean_revert") {
        strategy = std::make_unique<lxts::MeanReversionStrategy>();
    } else {
        strategy = std::make_unique<lxts::MomentumStrategy>();
    }

    // ── Wire fill callback ─────────────────────
    uint64_t seq = 0;
    om->set_fill_callback([&](const lxts::Fill& fill) {
        strategy->on_fill(fill);
        redis.push_fill(fill);
        redis.write_position(fill.symbol, strategy->position(fill.symbol),
                             strategy->realized_pnl(fill.symbol));
        std::cout << "[FILL] " << (fill.side == lxts::Side::BUY ? "BUY " : "SELL")
                  << " " << fill.symbol << " " << fill.fill_qty
                  << " @ " << std::fixed << std::setprecision(2) << fill.fill_price
                  << "  lat=" << fill.latency_us << "us\n";
    });

    // ── Market data feed ──────────────────────
    std::vector<std::string> symbols = {"AAPL", "SPY", "QQQ", "TSLA", "NVDA"};

    lxts::FeedPublisher pub(symbols);
    pub.start();
    std::cout << "[LXTS] Feed publisher started (multicast 239.1.1.1:9900)\n";

    lxts::FeedSubscriber sub;
    sub.set_book_callback([&](const lxts::OrderBook& book) {
        if (book.symbol != symbol) return;
        om->update_market_price(book.symbol, book.mid());
        strategy->on_book(book, *om);
        redis.write_price(book.symbol, book.mid());
        redis.write_seq(++seq);
    });
    sub.start();
    std::cout << "[LXTS] Feed subscriber started\n";

    // ── Main loop ─────────────────────────────
    std::cout << "[LXTS] Engine running. Ctrl-C to stop.\n";
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    redis.write_status("STOPPED");
    std::cout << "\n[LXTS] Shutting down.\n";
    return 0;
}
