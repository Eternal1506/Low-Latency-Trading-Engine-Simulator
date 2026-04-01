#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace lxts {

using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
using Price     = double;
using Qty       = int64_t;
using OrderID   = uint64_t;

inline Timestamp now() { return std::chrono::steady_clock::now(); }

inline int64_t micros_since(Timestamp t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(now() - t).count();
}

// ─────────────────────────────────────────────
// Market data structures
// ─────────────────────────────────────────────

struct PriceLevel {
    Price price;
    Qty   qty;
};

struct OrderBook {
    std::string symbol;
    std::vector<PriceLevel> bids; // sorted desc
    std::vector<PriceLevel> asks; // sorted asc
    Timestamp received_at;

    Price best_bid() const { return bids.empty() ? 0.0 : bids[0].price; }
    Price best_ask() const { return asks.empty() ? 0.0 : asks[0].price; }
    Price mid()      const { return (best_bid() + best_ask()) / 2.0; }
    Price spread()   const { return best_ask() - best_bid(); }
};

struct Trade {
    std::string symbol;
    Price       price;
    Qty         qty;
    bool        is_buy;
    Timestamp   timestamp;
};

// ─────────────────────────────────────────────
// Order structures
// ─────────────────────────────────────────────

enum class Side   { BUY, SELL };
enum class OrdType { MARKET, LIMIT };
enum class OrdStatus { PENDING, PARTIAL, FILLED, CANCELLED, REJECTED };

struct Order {
    OrderID   id;
    std::string symbol;
    Side      side;
    OrdType   type;
    Price     limit_price; // 0 for market
    Qty       qty;
    Qty       filled_qty  = 0;
    Price     avg_fill    = 0.0;
    OrdStatus status      = OrdStatus::PENDING;
    Timestamp created_at;
    Timestamp filled_at;
    int64_t   latency_us  = 0; // round-trip
};

struct Fill {
    OrderID   order_id;
    std::string symbol;
    Side      side;
    Price     fill_price;
    Qty       fill_qty;
    Timestamp filled_at;
    int64_t   latency_us;
};

// ─────────────────────────────────────────────
// Callbacks
// ─────────────────────────────────────────────

using BookCallback  = std::function<void(const OrderBook&)>;
using FillCallback  = std::function<void(const Fill&)>;
using TradeCallback = std::function<void(const Trade&)>;

} // namespace lxts
