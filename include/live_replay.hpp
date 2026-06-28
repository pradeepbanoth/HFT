#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// live_replay.hpp  —  Live WebSocket data replay into SimEngine
//
// Connects to Binance or Bybit WebSocket feeds and pipes the parsed
// events directly into a running SimEngine — identical to backtesting
// but with real-time data. Useful for:
//   • Paper trading validation
//   • Latency calibration against real exchange timing
//   • Real-time strategy monitoring (read-only)
//
// Design:
//   • POSIX socket + manual HTTP upgrade (no external websocket library)
//   • Non-blocking recv loop with nanosecond event timestamps
//   • Separate feed thread: parse → enqueue → SimEngine thread: process
//   • Lock-free SPSC ring buffer between threads (power-of-2 size)
//   • Graceful shutdown via std::atomic<bool> stop flag
//   • Reconnect with exponential backoff on disconnect
//
// Note: TLS is not implemented here (requires OpenSSL).
//       For production, wrap the socket in SSL_CTX or use a TLS proxy.
//       The class is structured so TlsSocket can be substituted for RawSocket.
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "simulator.hpp"
#include "parsers.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <array>
#include <cmath>

// POSIX headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// Lock-free SPSC Ring Buffer
// One producer (feed thread), one consumer (engine thread).
// Size must be power of 2.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
public:
    void push(T&& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) & mask_;
        while (next == tail_.load(std::memory_order_acquire)) {
            std::this_thread::yield();  // buffer full — back-pressure
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
    }

    bool pop(T& out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = std::move(buffer_[tail]);
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

private:
    static constexpr size_t mask_ = Capacity - 1;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::array<T, Capacity>         buffer_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket frame types
// ─────────────────────────────────────────────────────────────────────────────
enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

// ─────────────────────────────────────────────────────────────────────────────
// RawSocket  —  thin POSIX TCP socket wrapper
// ─────────────────────────────────────────────────────────────────────────────
class RawSocket {
public:
    RawSocket() : fd_(-1) {}
    ~RawSocket() { close(); }

    bool connect(const std::string& host, uint16_t port) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string port_str = std::to_string(port);

        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
            return false;

        fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0) { freeaddrinfo(res); return false; }

        // Set TCP_NODELAY for low latency
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        bool ok = (::connect(fd_, res->ai_addr, res->ai_addrlen) == 0);
        freeaddrinfo(res);
        if (!ok) { ::close(fd_); fd_ = -1; }
        return ok;
    }

    ssize_t send(const char* buf, size_t len) {
        return ::send(fd_, buf, len, MSG_NOSIGNAL);
    }

    ssize_t recv(char* buf, size_t len) {
        return ::recv(fd_, buf, len, 0);
    }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const { return fd_ >= 0; }
    int  fd() const { return fd_; }

private:
    int fd_;
};

// ─────────────────────────────────────────────────────────────────────────────
// WebSocketClient  —  minimal RFC 6455 client
// ─────────────────────────────────────────────────────────────────────────────
class WebSocketClient {
public:
    WebSocketClient() = default;

    // Connect and perform HTTP upgrade handshake
    bool connect(const std::string& host, uint16_t port, const std::string& path) {
        host_ = host; port_ = port; path_ = path;
        if (!sock_.connect(host, port)) return false;
        return do_handshake(host, path);
    }

    // Read the next complete WebSocket frame into `out`.
    // Returns true on success, false on error/close.
    bool read_frame(std::string& out, WsOpcode& opcode) {
        // Read 2-byte header
        uint8_t hdr[2];
        if (!recv_exact(reinterpret_cast<char*>(hdr), 2)) return false;

        bool fin      = (hdr[0] & 0x80) != 0;
        opcode        = static_cast<WsOpcode>(hdr[0] & 0x0F);
        bool masked   = (hdr[1] & 0x80) != 0;
        uint64_t len  = hdr[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!recv_exact(reinterpret_cast<char*>(ext), 2)) return false;
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!recv_exact(reinterpret_cast<char*>(ext), 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }

        // Read mask key (server→client is never masked per RFC, but handle it)
        uint8_t mask[4] = {};
        if (masked) {
            if (!recv_exact(reinterpret_cast<char*>(mask), 4)) return false;
        }

        // Read payload
        if (len > 16 * 1024 * 1024) return false;  // sanity: 16MB max
        out.resize(len);
        if (len > 0 && !recv_exact(out.data(), len)) return false;

        if (masked) {
            for (size_t i = 0; i < len; ++i)
                out[i] ^= mask[i % 4];
        }

        return true;
    }

    // Send a text WebSocket frame (client→server, with masking)
    bool send_text(const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + Text opcode

        size_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(0x80 | len));
        } else if (len <= 65535) {
            frame.push_back(0xFE);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(0xFF);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
        }

        // Client masking key (random 4 bytes)
        uint8_t mask[4];
        uint32_t rk = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        memcpy(mask, &rk, 4);
        for (uint8_t b : mask) frame.push_back(b);

        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);

        return sock_.send(reinterpret_cast<const char*>(frame.data()), frame.size())
               == static_cast<ssize_t>(frame.size());
    }

    // Send a pong frame (keep-alive response to server ping)
    bool send_pong(const std::string& payload = "") {
        uint8_t hdr[2] = {0x8A, static_cast<uint8_t>(payload.size())};
        sock_.send(reinterpret_cast<const char*>(hdr), 2);
        if (!payload.empty())
            sock_.send(payload.data(), payload.size());
        return true;
    }

    void disconnect() { sock_.close(); }
    bool is_connected() const { return sock_.is_open(); }
    const std::string& host() const { return host_; }
    const std::string& path() const { return path_; }

private:
    RawSocket   sock_;
    std::string host_, path_;
    uint16_t    port_ = 0;

    bool do_handshake(const std::string& host, const std::string& path) {
        // Send HTTP/1.1 upgrade request
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "\r\n";

        std::string req_str = req.str();
        if (sock_.send(req_str.data(), req_str.size()) != (ssize_t)req_str.size())
            return false;

        // Read response until \r\n\r\n
        std::string resp;
        char ch;
        while (resp.size() < 4096) {
            if (sock_.recv(&ch, 1) != 1) return false;
            resp += ch;
            if (resp.size() >= 4 &&
                resp.substr(resp.size() - 4) == "\r\n\r\n") break;
        }
        return resp.find("101") != std::string::npos;
    }

    bool recv_exact(char* buf, size_t n) {
        size_t received = 0;
        while (received < n) {
            ssize_t r = sock_.recv(buf + received, n - received);
            if (r <= 0) return false;
            received += static_cast<size_t>(r);
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LiveFeedConfig  —  exchange + symbol configuration
// ─────────────────────────────────────────────────────────────────────────────
enum class Exchange { Binance, Bybit };

struct LiveFeedConfig {
    Exchange    exchange        = Exchange::Binance;
    std::string symbol         = "BTCUSDT";
    bool        subscribe_book = true;
    bool        subscribe_trades= true;
    int         book_depth     = 20;    // 5/10/20 for Binance; 1/50/200 for Bybit
    bool        verbose        = false;
    int         reconnect_delay_s = 5;
    int         max_reconnects   = 10;
};

// Build the WebSocket path and host for a given config
inline std::pair<std::string,std::string>
build_ws_endpoint(const LiveFeedConfig& cfg) {
    std::string host, path;
    std::string sym_lower = cfg.symbol;
    for (char& c : sym_lower) c = static_cast<char>(std::tolower(c));

    if (cfg.exchange == Exchange::Binance) {
        // Binance spot combined stream
        host = "stream.binance.com";
        std::vector<std::string> streams;
        if (cfg.subscribe_book)
            streams.push_back(sym_lower + "@depth" + std::to_string(cfg.book_depth) + "@100ms");
        if (cfg.subscribe_trades)
            streams.push_back(sym_lower + "@aggTrade");
        path = "/stream?streams=";
        for (size_t i = 0; i < streams.size(); ++i) {
            if (i) path += "/";
            path += streams[i];
        }
    } else {
        // Bybit V5 public stream
        host = "stream.bybit.com";
        path = "/v5/public/spot";
    }
    return {host, path};
}

// Build subscribe message for Bybit
inline std::string bybit_subscribe_msg(const LiveFeedConfig& cfg) {
    std::ostringstream oss;
    oss << R"({"op":"subscribe","args":[)";
    bool first = true;
    if (cfg.subscribe_book) {
        oss << "\"orderbook." << cfg.book_depth << "." << cfg.symbol << "\"";
        first = false;
    }
    if (cfg.subscribe_trades) {
        if (!first) oss << ",";
        oss << "\"publicTrade." << cfg.symbol << "\"";
    }
    oss << "]}";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// LiveReplay  —  feeds real exchange data into a Strategy via SimEngine
// ─────────────────────────────────────────────────────────────────────────────
class LiveReplay {
public:
    static constexpr size_t kRingSize = 65536;   // 64K event buffer

    explicit LiveReplay(
        Strategy&         strategy,
        LatencyProfile    latency,
        FillModelConfig   fill_cfg  = {},
        double            initial_cash = 100'000.0,
        RiskLimits        limits    = {}
    )
        : engine_(strategy, latency, fill_cfg, initial_cash, limits)
    {}

    // Add a symbol to trade. Call before start().
    void add_symbol(const std::string& sym, double tick_size = 1e-8) {
        engine_.add_symbol(sym, tick_size);
        symbols_.push_back(sym);
    }

    // Connect to exchange and start processing.
    // Blocks until stop() is called or max_reconnects exceeded.
    void start(const LiveFeedConfig& cfg) {
        stop_flag_.store(false);
        cfg_ = cfg;

        std::cout << "[LiveReplay] Starting live feed: "
                  << cfg.symbol << " on "
                  << (cfg.exchange == Exchange::Binance ? "Binance" : "Bybit") << "\n";

        // Create parser
        if (cfg.exchange == Exchange::Binance) {
            binance_parser_ = std::make_unique<BinanceParser>(cfg.symbol);
        } else {
            bybit_parser_ = std::make_unique<BybitParser>(cfg.symbol);
        }

        // Start engine thread
        engine_thread_ = std::thread([this]{ engine_loop(); });

        // Feed thread (this thread)
        int reconnects = 0;
        while (!stop_flag_.load() && reconnects <= cfg.max_reconnects) {
            if (!connect_and_feed()) {
                if (stop_flag_.load()) break;
                ++reconnects;
                int delay = cfg.reconnect_delay_s * (1 << std::min(reconnects - 1, 5));
                std::cerr << "[LiveReplay] Disconnected. Reconnect " << reconnects
                          << "/" << cfg.max_reconnects
                          << " in " << delay << "s\n";
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            } else {
                reconnects = 0;
            }
        }

        stop_flag_.store(true);
        if (engine_thread_.joinable()) engine_thread_.join();
        std::cout << "[LiveReplay] Stopped.\n";
    }

    void stop() { stop_flag_.store(true); }

    SimEngine& engine() { return engine_; }

    // Telemetry
    struct Stats {
        int64_t messages_received  = 0;
        int64_t events_parsed      = 0;
        int64_t events_dropped     = 0;  // ring buffer full
        int64_t events_processed   = 0;
        double  avg_parse_us       = 0.0;
        double  avg_process_us     = 0.0;
    };
    Stats stats() const { return stats_; }

private:
    SimEngine                       engine_;
    std::vector<std::string>        symbols_;
    LiveFeedConfig                  cfg_;
    std::atomic<bool>               stop_flag_{false};
    std::thread                     engine_thread_;
    SPSCRingBuffer<MarketEvent, kRingSize> ring_;
    mutable Stats                   stats_;

    std::unique_ptr<BinanceParser>  binance_parser_;
    std::unique_ptr<BybitParser>    bybit_parser_;

    // ── Feed thread ───────────────────────────────────────────────────────────

    bool connect_and_feed() {
        auto [host, path] = build_ws_endpoint(cfg_);

        if (cfg_.verbose)
            std::cout << "[LiveReplay] Connecting to " << host << path << "\n";

        WebSocketClient ws;
        if (!ws.connect(host, 80, path)) {  // Use 443 + TLS for production
            std::cerr << "[LiveReplay] Connection failed to " << host << "\n";
            return false;
        }

        std::cout << "[LiveReplay] Connected to " << host << "\n";

        // Send subscribe message for Bybit
        if (cfg_.exchange == Exchange::Bybit) {
            ws.send_text(bybit_subscribe_msg(cfg_));
        }

        // Read loop
        std::string frame_data;
        WsOpcode    opcode;

        while (!stop_flag_.load() && ws.is_connected()) {
            if (!ws.read_frame(frame_data, opcode)) break;

            if (opcode == WsOpcode::Close)  break;
            if (opcode == WsOpcode::Ping) { ws.send_pong(frame_data); continue; }
            if (opcode != WsOpcode::Text && opcode != WsOpcode::Binary) continue;

            ++stats_.messages_received;

            // Parse message → events
            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<MarketEvent> events;

            if (cfg_.exchange == Exchange::Binance && binance_parser_) {
                auto parsed = binance_parser_->parse(frame_data);
                events = std::move(parsed);
            } else if (cfg_.exchange == Exchange::Bybit && bybit_parser_) {
                auto parsed = bybit_parser_->parse(frame_data);
                events = std::move(parsed);
            }

            auto t1 = std::chrono::high_resolution_clock::now();
            double parse_us = std::chrono::duration<double,std::micro>(t1-t0).count();
            stats_.avg_parse_us = stats_.avg_parse_us * 0.99 + parse_us * 0.01;

            // Enqueue events into ring buffer
            for (auto& evt : events) {
                ++stats_.events_parsed;
                // Stamp with receive time if not already set
                int64_t ts = event_timestamp(evt);
                if (ts == 0) {
                    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    // Update timestamp in variant
                    std::visit([now](auto& e){ e.timestamp = now; }, evt);
                }

                if (ring_.size() < kRingSize - 1) {
                    ring_.push(std::move(evt));
                } else {
                    ++stats_.events_dropped;
                    if (cfg_.verbose)
                        std::cerr << "[LiveReplay] WARNING: ring buffer full, dropping event\n";
                }
            }
        }

        ws.disconnect();
        return !stop_flag_.load();
    }

    // ── Engine thread ─────────────────────────────────────────────────────────

    void engine_loop() {
        // We drive the engine manually: pop events from ring and feed one by one.
        // Since SimEngine::run() expects an iterable, we use a lambda-based generator
        // that reads from the ring buffer.

        std::cout << "[LiveReplay] Engine thread started\n";

        // Simple manual event loop (not using SimEngine::run to avoid blocking)
        engine_.on_start_manual();

        while (!stop_flag_.load() || !ring_.empty()) {
            MarketEvent evt;
            if (!ring_.pop(evt)) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            auto t0 = std::chrono::high_resolution_clock::now();
            engine_.process_one(evt);
            auto t1 = std::chrono::high_resolution_clock::now();
            double proc_us = std::chrono::duration<double,std::micro>(t1-t0).count();
            stats_.avg_process_us = stats_.avg_process_us * 0.99 + proc_us * 0.01;
            ++stats_.events_processed;

            if (cfg_.verbose && stats_.events_processed % 10000 == 0) {
                std::cout << "[LiveReplay] events=" << stats_.events_processed
                          << " parse=" << std::fixed << std::setprecision(2)
                          << stats_.avg_parse_us << "µs"
                          << " proc=" << stats_.avg_process_us << "µs\n";
            }
        }

        engine_.on_end_manual();
        std::cout << "[LiveReplay] Engine thread stopped. Events processed: "
                  << stats_.events_processed << "\n";
    }
};

} // namespace hft