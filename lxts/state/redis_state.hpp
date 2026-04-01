#pragma once
#include "../engine/engine.hpp"
#include <hiredis/hiredis.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace lxts {

// ─────────────────────────────────────────────
// RedisStateManager
// Persists engine state to Redis after every fill.
// Keys:
//   engine:seq          → current sequence number
//   engine:status       → ACTIVE | PAUSED
//   state:{SYM}:price   → last mid price
//   state:{SYM}:pos     → current position (shares)
//   state:{SYM}:pnl     → realized P&L
//   fills               → Redis list, LPUSH each fill (capped at 1000)
// ─────────────────────────────────────────────

class RedisStateManager {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int         port = 6379;
        int         db   = 0;
    };

    RedisStateManager() : RedisStateManager(Config{}) {}

    explicit RedisStateManager(Config cfg) : cfg_(cfg) {
        connect();
    }

    ~RedisStateManager() {
        if (ctx_) redisFree(ctx_);
    }

    bool is_connected() const { return ctx_ && ctx_->err == 0; }

    // Call after each book update
    void write_price(const std::string& sym, double price) {
        cmd("SET state:%s:price %.4f", sym.c_str(), price);
    }

    // Call after each fill
    void write_position(const std::string& sym, int64_t pos, double pnl) {
        cmd("SET state:%s:pos %lld",  sym.c_str(), (long long)pos);
        cmd("SET state:%s:pnl %.2f",  sym.c_str(), pnl);
    }

    void write_seq(uint64_t seq) {
        cmd("SET engine:seq %llu", (unsigned long long)seq);
    }

    void write_status(const std::string& status) {
        cmd("SET engine:status %s", status.c_str());
    }

    void push_fill(const Fill& f) {
        std::ostringstream oss;
        oss << (f.side == Side::BUY ? "BUY" : "SELL")
            << " " << f.symbol
            << " " << f.fill_qty << "@" << std::fixed << std::setprecision(2) << f.fill_price
            << " lat=" << f.latency_us << "us";
        cmd("LPUSH fills %s", oss.str().c_str());
        cmd("LPUSH trades %s", oss.str().c_str());
        cmd("LTRIM fills 0 999"); // keep last 1000
        cmd("LTRIM trades 0 999");
    }

    // Read helpers (for Python analysis tools)
    std::string get(const std::string& key) {
        if (!ctx_) return "";
        auto* r = static_cast<redisReply*>(redisCommand(ctx_, "GET %s", key.c_str()));
        if (!r) return "";
        std::string val = (r->type == REDIS_REPLY_STRING) ? r->str : "";
        freeReplyObject(r);
        return val;
    }

private:
    void connect() {
        ctx_ = redisConnect(cfg_.host.c_str(), cfg_.port);
        if (!ctx_ || ctx_->err) {
            fprintf(stderr, "[Redis] Connection failed: %s\n",
                    ctx_ ? ctx_->errstr : "OOM");
            if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        } else {
            fprintf(stderr, "[Redis] Connected to %s:%d\n",
                    cfg_.host.c_str(), cfg_.port);
            if (cfg_.db != 0) cmd("SELECT %d", cfg_.db);
        }
    }

    template<typename... Args>
    void cmd(const char* fmt, Args... args) {
        if (!ctx_) return;
        auto* r = static_cast<redisReply*>(redisCommand(ctx_, fmt, args...));
        if (r) freeReplyObject(r);
        else { fprintf(stderr, "[Redis] Command failed\n"); reconnect(); }
    }

    void reconnect() {
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        connect();
    }

    Config cfg_;
    redisContext* ctx_ = nullptr;
};

} // namespace lxts
