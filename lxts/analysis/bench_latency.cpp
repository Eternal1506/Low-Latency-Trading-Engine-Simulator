#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>
#include <algorithm>
#include <thread>

// ─────────────────────────────────────────────
// Standalone latency benchmark
// Measures round-trip time of order submission
// through the in-process order manager.
// ─────────────────────────────────────────────

#include "../engine/engine.hpp"
#include "../engine/order_manager.hpp"

int main() {
    printf("LXTS Latency Benchmark\n");
    printf("======================\n\n");

    lxts::OrderManager::Config cfg;
    cfg.min_latency_us = 10;
    cfg.max_latency_us = 80;
    lxts::OrderManager om(cfg);
    om.update_market_price("AAPL", 213.50);

    const int N = 10000;
    std::vector<int64_t> latencies;
    latencies.reserve(N);

    std::atomic<int> filled{0};
    om.set_fill_callback([&](const lxts::Fill& f) {
        latencies.push_back(f.latency_us);
        filled++;
    });

    // Submit N market orders and record times
    for (int i = 0; i < N; i++) {
        om.submit("AAPL", lxts::Side::BUY, lxts::OrdType::MARKET, 100);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    // Wait for all fills
    while (filled.load() < N)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::sort(latencies.begin(), latencies.end());
    double mean = std::accumulate(latencies.begin(), latencies.end(), 0LL) / (double)N;

    printf("Orders:  %d\n", N);
    printf("Mean:    %.1f us\n", mean);
    printf("Median:  %lld us\n", (long long)latencies[N/2]);
    printf("p95:     %lld us\n", (long long)latencies[(int)(N*0.95)]);
    printf("p99:     %lld us\n", (long long)latencies[(int)(N*0.99)]);
    printf("p99.9:   %lld us\n", (long long)latencies[(int)(N*0.999)]);
    printf("Max:     %lld us\n", (long long)latencies.back());

    return 0;
}
