#pragma once
#include "engine.hpp"
#include <mutex>
#include <queue>
#include <random>
#include <thread>

namespace lxts {

// ─────────────────────────────────────────────
// Simulated Order Manager
// Mimics exchange round-trip with configurable
// latency injection and fill simulation.
// ─────────────────────────────────────────────

class OrderManager {
public:
    struct Config {
        int64_t min_latency_us = 20;    // min simulated exchange RTT
        int64_t max_latency_us = 120;   // max simulated exchange RTT
        double  fill_rate      = 0.97;  // probability of fill (market orders)
        double  slip_bps       = 0.5;   // max slippage in basis points
    };

    OrderManager() : OrderManager(Config{}) {}

    explicit OrderManager(Config cfg) : cfg_(cfg), next_id_(1) {
        worker_ = std::thread(&OrderManager::process_loop, this);
    }

    ~OrderManager() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    void set_fill_callback(FillCallback cb) { fill_cb_ = std::move(cb); }

    // Submit order – non-blocking, returns order id immediately
    OrderID submit(const std::string& symbol, Side side, OrdType type,
                   Qty qty, Price limit_price = 0.0) {
        auto ord        = std::make_shared<Order>();
        ord->id         = next_id_++;
        ord->symbol     = symbol;
        ord->side       = side;
        ord->type       = type;
        ord->qty        = qty;
        ord->limit_price= limit_price;
        ord->created_at = now();

        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push(ord);
            orders_[ord->id] = ord;
        }
        return ord->id;
    }

    bool cancel(OrderID id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        if (it->second->status == OrdStatus::PENDING) {
            it->second->status = OrdStatus::CANCELLED;
            return true;
        }
        return false;
    }

    std::shared_ptr<Order> get_order(OrderID id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = orders_.find(id);
        return (it != orders_.end()) ? it->second : nullptr;
    }

    // Called by feed with latest mid price so simulation can compute fills
    void update_market_price(const std::string& sym, Price mid) {
        std::lock_guard<std::mutex> lk(price_mu_);
        market_prices_[sym] = mid;
    }

private:
    void process_loop() {
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<int64_t> lat_dist(cfg_.min_latency_us, cfg_.max_latency_us);
        std::uniform_real_distribution<double> slip_dist(0.0, cfg_.slip_bps / 10000.0);
        std::uniform_real_distribution<double> prob_dist(0.0, 1.0);

        while (!stop_.load()) {
            std::shared_ptr<Order> ord;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!pending_.empty()) {
                    ord = pending_.front();
                    pending_.pop();
                }
            }

            if (!ord) { std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
            if (ord->status == OrdStatus::CANCELLED) continue;

            // Simulate network + exchange latency
            int64_t lat = lat_dist(rng);
            std::this_thread::sleep_for(std::chrono::microseconds(lat));

            Price mid = 0.0;
            {
                std::lock_guard<std::mutex> lk(price_mu_);
                auto it = market_prices_.find(ord->symbol);
                if (it != market_prices_.end()) mid = it->second;
            }

            if (mid == 0.0 || prob_dist(rng) > cfg_.fill_rate) {
                std::lock_guard<std::mutex> lk(mu_);
                ord->status = OrdStatus::REJECTED;
                continue;
            }

            // Compute fill price with slippage
            double slip = slip_dist(rng);
            Price fill_price = (ord->side == Side::BUY)
                ? mid * (1.0 + slip)
                : mid * (1.0 - slip);

            // Limit order: check price
            if (ord->type == OrdType::LIMIT) {
                if (ord->side == Side::BUY  && fill_price > ord->limit_price) continue;
                if (ord->side == Side::SELL && fill_price < ord->limit_price) continue;
            }

            Fill fill;
            fill.order_id   = ord->id;
            fill.symbol     = ord->symbol;
            fill.side       = ord->side;
            fill.fill_price = fill_price;
            fill.fill_qty   = ord->qty;
            fill.filled_at  = now();
            fill.latency_us = lat;

            {
                std::lock_guard<std::mutex> lk(mu_);
                ord->status     = OrdStatus::FILLED;
                ord->filled_qty = ord->qty;
                ord->avg_fill   = fill_price;
                ord->filled_at  = fill.filled_at;
                ord->latency_us = lat;
            }

            if (fill_cb_) fill_cb_(fill);
        }
    }

    Config cfg_;
    std::atomic<OrderID> next_id_;
    std::atomic<bool>    stop_{false};

    std::mutex mu_;
    std::queue<std::shared_ptr<Order>> pending_;
    std::unordered_map<OrderID, std::shared_ptr<Order>> orders_;

    std::mutex price_mu_;
    std::unordered_map<std::string, Price> market_prices_;

    FillCallback fill_cb_;
    std::thread  worker_;
};

} // namespace lxts
