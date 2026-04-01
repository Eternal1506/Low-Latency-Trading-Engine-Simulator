#pragma once
#include "../engine/engine.hpp"
#include "../engine/order_manager.hpp"
#include <deque>
#include <numeric>

namespace lxts {

// ─────────────────────────────────────────────
// Strategy interface
// ─────────────────────────────────────────────

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_book(const OrderBook& book, OrderManager& om) = 0;
    virtual void on_fill(const Fill& fill) = 0;
    virtual std::string name() const = 0;

    int64_t position(const std::string& sym) const {
        auto it = positions_.find(sym);
        return it != positions_.end() ? it->second : 0;
    }

    double realized_pnl(const std::string& sym) const {
        auto it = realized_pnl_by_symbol_.find(sym);
        return it != realized_pnl_by_symbol_.end() ? it->second : 0.0;
    }

    // Max position guard (absolute shares)
    int64_t max_position = 1000;

protected:
    std::unordered_map<std::string, int64_t> positions_;
    std::unordered_map<std::string, double>  avg_costs_;
    std::unordered_map<std::string, double>  realized_pnl_by_symbol_;
    double realized_pnl_ = 0.0;

    void apply_fill(const Fill& f) {
        int64_t& pos  = positions_[f.symbol];
        double&  cost = avg_costs_[f.symbol];
        int64_t qty   = (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;

        if ((qty > 0 && pos < 0) || (qty < 0 && pos > 0)) {
            int64_t close_qty = std::min(std::abs(qty), std::abs(pos));
            double pnl = close_qty * (f.fill_price - cost) * (pos > 0 ? 1 : -1);
            realized_pnl_ += pnl;
            realized_pnl_by_symbol_[f.symbol] += pnl;
        }
        if (pos == 0) cost = f.fill_price;
        else if ((qty > 0 && pos >= 0) || (qty < 0 && pos <= 0))
            cost = (cost * std::abs(pos) + f.fill_price * std::abs(qty)) / (std::abs(pos) + std::abs(qty));
        pos += qty;
    }

    bool can_buy(const std::string& sym, int64_t qty) const {
        auto it = positions_.find(sym);
        int64_t pos = (it != positions_.end()) ? it->second : 0;
        return pos + qty <= max_position;
    }
    bool can_sell(const std::string& sym, int64_t qty) const {
        auto it = positions_.find(sym);
        int64_t pos = (it != positions_.end()) ? it->second : 0;
        return pos - qty >= -max_position;
    }
};

// ─────────────────────────────────────────────
// 1. Market Making
// Quotes two-sided around mid, earns spread.
// ─────────────────────────────────────────────

class MarketMaker : public Strategy {
public:
    struct Config {
        double quote_offset = 0.02;  // distance from mid to quote
        int64_t quote_qty   = 100;
        int64_t min_spread  = 0;     // skip quoting if spread < this (bps)
    };

    MarketMaker() : MarketMaker(Config{}) {}

    explicit MarketMaker(Config cfg) : cfg_(cfg) {}
    std::string name() const override { return "MarketMaker"; }

    void on_book(const OrderBook& book, OrderManager& om) override {
        double mid = book.mid();
        if (mid <= 0) return;

        auto& sym = book.symbol;

        // Only re-quote every N ticks to avoid spamming
        auto& cnt = tick_count_[sym];
        if (++cnt % 5 != 0) return;

        if (can_buy(sym, cfg_.quote_qty)) {
            om.submit(sym, Side::BUY, OrdType::LIMIT, cfg_.quote_qty, mid - cfg_.quote_offset);
        }
        if (can_sell(sym, cfg_.quote_qty)) {
            om.submit(sym, Side::SELL, OrdType::LIMIT, cfg_.quote_qty, mid + cfg_.quote_offset);
        }
    }

    void on_fill(const Fill& f) override { apply_fill(f); }

private:
    Config cfg_;
    std::unordered_map<std::string, int64_t> tick_count_;
};

// ─────────────────────────────────────────────
// 2. Momentum (EMA crossover)
// Buy when fast EMA > slow EMA, sell opposite.
// ─────────────────────────────────────────────

class MomentumStrategy : public Strategy {
public:
    struct Config {
        int     fast_period = 10;
        int     slow_period = 30;
        int64_t trade_qty   = 100;
    };

    MomentumStrategy() : MomentumStrategy(Config{}) {}

    explicit MomentumStrategy(Config cfg) : cfg_(cfg) {}
    std::string name() const override { return "Momentum"; }

    void on_book(const OrderBook& book, OrderManager& om) override {
        double mid = book.mid();
        if (mid <= 0) return;

        auto& sym = book.symbol;
        auto& hist = history_[sym];
        hist.push_back(mid);
        if ((int)hist.size() > cfg_.slow_period * 2) hist.pop_front();
        if ((int)hist.size() < cfg_.slow_period) return;

        double fast = ema(hist, cfg_.fast_period);
        double slow = ema(hist, cfg_.slow_period);

        bool& in_trade = in_trade_[sym];

        if (fast > slow * 1.0002 && !in_trade && can_buy(sym, cfg_.trade_qty)) {
            om.submit(sym, Side::BUY, OrdType::MARKET, cfg_.trade_qty);
            in_trade = true;
        } else if (fast < slow * 0.9998 && in_trade && can_sell(sym, cfg_.trade_qty)) {
            om.submit(sym, Side::SELL, OrdType::MARKET, cfg_.trade_qty);
            in_trade = false;
        }
    }

    void on_fill(const Fill& f) override { apply_fill(f); }

private:
    Config cfg_;
    std::unordered_map<std::string, std::deque<double>> history_;
    std::unordered_map<std::string, bool> in_trade_;

    static double ema(const std::deque<double>& data, int period) {
        double k = 2.0 / (period + 1);
        double e = data[data.size() - period];
        for (int i = (int)data.size() - period + 1; i < (int)data.size(); i++)
            e = data[i] * k + e * (1 - k);
        return e;
    }
};

// ─────────────────────────────────────────────
// 3. Mean Reversion (Bollinger Bands)
// Fade price moves that exceed N std devs.
// ─────────────────────────────────────────────

class MeanReversionStrategy : public Strategy {
public:
    struct Config {
        int     lookback    = 20;
        double  num_std     = 2.0;
        int64_t trade_qty   = 100;
    };

    MeanReversionStrategy() : MeanReversionStrategy(Config{}) {}

    explicit MeanReversionStrategy(Config cfg) : cfg_(cfg) {}
    std::string name() const override { return "MeanReversion"; }

    void on_book(const OrderBook& book, OrderManager& om) override {
        double mid = book.mid();
        if (mid <= 0) return;

        auto& sym = book.symbol;
        auto& hist = history_[sym];
        hist.push_back(mid);
        if ((int)hist.size() > cfg_.lookback * 2) hist.pop_front();
        if ((int)hist.size() < cfg_.lookback) return;

        // Compute mean and std over lookback window
        auto begin = hist.end() - cfg_.lookback;
        std::vector<double> w(begin, hist.end());
        double mean = std::accumulate(w.begin(), w.end(), 0.0) / w.size();
        double var  = 0;
        for (double v : w) var += (v - mean) * (v - mean);
        double sd = std::sqrt(var / w.size());

        double upper = mean + cfg_.num_std * sd;
        double lower = mean - cfg_.num_std * sd;

        auto& pos = positions_[sym];

        if (mid > upper && can_sell(sym, cfg_.trade_qty)) {
            om.submit(sym, Side::SELL, OrdType::MARKET, cfg_.trade_qty);
        } else if (mid < lower && can_buy(sym, cfg_.trade_qty)) {
            om.submit(sym, Side::BUY, OrdType::MARKET, cfg_.trade_qty);
        } else if (mid > mean * 0.999 && mid < mean * 1.001 && pos != 0) {
            // Unwind near mean
            if (pos > 0) om.submit(sym, Side::SELL, OrdType::MARKET, std::abs(pos));
            else         om.submit(sym, Side::BUY,  OrdType::MARKET, std::abs(pos));
        }
    }

    void on_fill(const Fill& f) override { apply_fill(f); }

private:
    Config cfg_;
    std::unordered_map<std::string, std::deque<double>> history_;
};

} // namespace lxts
