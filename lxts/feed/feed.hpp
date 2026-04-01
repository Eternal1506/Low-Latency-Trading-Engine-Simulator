#pragma once
#include "../engine/engine.hpp"
#include <cmath>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle_t = SOCKET;
constexpr socket_handle_t invalid_socket_handle = INVALID_SOCKET;
inline void close_socket(socket_handle_t sock) { closesocket(sock); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle_t = int;
constexpr socket_handle_t invalid_socket_handle = -1;
inline void close_socket(socket_handle_t sock) { close(sock); }
#endif

namespace lxts {

// ─────────────────────────────────────────────
// UDP Market Data Feed
//
// Publishes simulated L2 order book snapshots
// and last-sale ticks over multicast UDP.
// Wire format (CSV for simplicity):
//   BOOK,AAPL,bid,213.50,100,213.49,200,...,ask,213.51,150,...
//   TRADE,AAPL,213.50,300,B
// ─────────────────────────────────────────────

class FeedPublisher {
public:
    struct Config {
        std::string mcast_group = "239.1.1.1";
        uint16_t    port        = 9900;
        int         levels      = 5;
        int         tick_ms     = 50; // publish every 50ms (~20 Hz)
    };

    FeedPublisher(std::vector<std::string> symbols)
        : FeedPublisher(std::move(symbols), Config{}) {}

    FeedPublisher(std::vector<std::string> symbols, Config cfg)
        : symbols_(std::move(symbols)), cfg_(cfg) {
        // initialize mid prices
        std::mt19937 rng(42);
        for (auto& s : symbols_) {
            mids_[s]  = base_price(s);
            vols_[s]  = base_vol(s);
        }
        setup_socket();
    }

    ~FeedPublisher() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (sock_ != invalid_socket_handle) close_socket(sock_);
    #ifdef _WIN32
        WSACleanup();
    #endif
    }

    void start() {
        worker_ = std::thread(&FeedPublisher::publish_loop, this);
    }

private:
    static double base_price(const std::string& s) {
        if (s == "AAPL")  return 213.50;
        if (s == "SPY")   return 528.00;
        if (s == "QQQ")   return 452.00;
        if (s == "TSLA")  return 182.00;
        if (s == "NVDA")  return 875.00;
        return 100.0;
    }
    static double base_vol(const std::string& s) {
        if (s == "TSLA" || s == "NVDA") return 0.15;
        if (s == "AAPL") return 0.06;
        return 0.04;
    }

    void setup_socket() {
#ifdef _WIN32
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            perror("WSAStartup");
            return;
        }
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == invalid_socket_handle) { perror("socket"); return; }

        int ttl = 1;
        setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

        memset(&dest_, 0, sizeof(dest_));
        dest_.sin_family      = AF_INET;
        dest_.sin_port        = htons(cfg_.port);
        dest_.sin_addr.s_addr = inet_addr(cfg_.mcast_group.c_str());
    }

    void send_msg(const std::string& msg) {
        if (sock_ == invalid_socket_handle) return;
        sendto(sock_, msg.c_str(), msg.size(), 0,
               reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
    }

    void publish_loop() {
        std::mt19937_64 rng(std::random_device{}());
        std::normal_distribution<double> drift(0.0, 1.0);
        std::uniform_real_distribution<double> spread_d(0.01, 0.05);
        std::uniform_int_distribution<int>     size_d(100, 5000);

        while (!stop_.load()) {
            for (auto& sym : symbols_) {
                double vol = vols_[sym];
                double& mid = mids_[sym];

                // GBM price step
                mid *= std::exp(vol * drift(rng) * std::sqrt(cfg_.tick_ms / 1e3 / 252.0));

                // Build book message
                std::ostringstream oss;
                oss << "BOOK," << sym;
                double bid = mid - spread_d(rng);
                double ask = mid + spread_d(rng);
                oss << ",bid";
                for (int i = 0; i < cfg_.levels; i++) {
                    oss << "," << std::fixed << std::setprecision(2) << (bid - i * 0.01)
                        << "," << size_d(rng);
                }
                oss << ",ask";
                for (int i = 0; i < cfg_.levels; i++) {
                    oss << "," << std::fixed << std::setprecision(2) << (ask + i * 0.01)
                        << "," << size_d(rng);
                }
                send_msg(oss.str());

                // Occasionally emit a trade tick
                if (drift(rng) > 1.5) {
                    std::ostringstream t;
                    t << "TRADE," << sym << ","
                      << std::fixed << std::setprecision(2) << mid << ","
                      << size_d(rng) << "," << (drift(rng) > 0 ? "B" : "S");
                    send_msg(t.str());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.tick_ms));
        }
    }

    std::vector<std::string> symbols_;
    Config cfg_;
    std::unordered_map<std::string, double> mids_;
    std::unordered_map<std::string, double> vols_;

    socket_handle_t sock_ = invalid_socket_handle;
    sockaddr_in dest_{};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

// ─────────────────────────────────────────────
// UDP Feed Subscriber / Parser
// ─────────────────────────────────────────────

class FeedSubscriber {
public:
    struct Config {
        std::string mcast_group = "239.1.1.1";
        std::string iface       = "0.0.0.0";
        uint16_t    port        = 9900;
    };

    FeedSubscriber() : FeedSubscriber(Config{}) {}

    FeedSubscriber(Config cfg) : cfg_(cfg) { setup_socket(); }

    ~FeedSubscriber() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (sock_ != invalid_socket_handle) close_socket(sock_);
    #ifdef _WIN32
        WSACleanup();
    #endif
    }

    void set_book_callback(BookCallback  cb) { book_cb_  = std::move(cb); }
    void set_trade_callback(TradeCallback cb) { trade_cb_ = std::move(cb); }

    void start() {
        worker_ = std::thread(&FeedSubscriber::recv_loop, this);
    }

private:
    void setup_socket() {
#ifdef _WIN32
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            perror("WSAStartup");
            return;
        }
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == invalid_socket_handle) { perror("socket"); return; }

        int reuse = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(cfg_.port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("bind"); return;
        }

        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(cfg_.mcast_group.c_str());
        mreq.imr_interface.s_addr = inet_addr(cfg_.iface.c_str());
        setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    }

    std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, d)) out.push_back(tok);
        return out;
    }

    void parse(const std::string& msg) {
        auto f = split(msg, ',');
        if (f.empty()) return;

        if (f[0] == "BOOK" && f.size() >= 3) {
            OrderBook book;
            book.symbol      = f[1];
            book.received_at = now();
            bool in_bid = false, in_ask = false;
            for (size_t i = 2; i < f.size(); i++) {
                if (f[i] == "bid") { in_bid = true; in_ask = false; continue; }
                if (f[i] == "ask") { in_ask = true; in_bid = false; continue; }
                if (in_bid && i + 1 < f.size()) {
                    book.bids.push_back({std::stod(f[i]), std::stoll(f[i+1])}); i++;
                } else if (in_ask && i + 1 < f.size()) {
                    book.asks.push_back({std::stod(f[i]), std::stoll(f[i+1])}); i++;
                }
            }
            if (book_cb_) book_cb_(book);
        } else if (f[0] == "TRADE" && f.size() >= 5) {
            Trade t;
            t.symbol    = f[1];
            t.price     = std::stod(f[2]);
            t.qty       = std::stoll(f[3]);
            t.is_buy    = (f[4] == "B");
            t.timestamp = now();
            if (trade_cb_) trade_cb_(t);
        }
    }

    void recv_loop() {
        char buf[4096];
        while (!stop_.load()) {
            auto n = recv(sock_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) continue;
            buf[static_cast<size_t>(n)] = '\0';
            parse(std::string(buf, static_cast<size_t>(n)));
        }
    }

    Config cfg_;
    socket_handle_t sock_ = invalid_socket_handle;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    BookCallback  book_cb_;
    TradeCallback trade_cb_;
};

} // namespace lxts
