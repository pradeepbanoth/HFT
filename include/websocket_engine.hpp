#pragma once
// websocket_engine.hpp — advanced mockable WebSocket engine layer

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hft {

enum class WsState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closing,
    Error
};

enum class WsMessageType : uint8_t {
    Text,
    Binary,
    Ping,
    Pong,
    Close
};

enum class WsEventType : uint8_t {
    Connected,
    Disconnected,
    Reconnecting,
    Message,
    Ping,
    Pong,
    Close,
    Error,
    Stale,
    SubscriptionAdded,
    SubscriptionRemoved
};

struct WsMessage {
    WsMessageType type = WsMessageType::Text;
    std::string payload;
    int64_t recv_ts = 0;
};

struct WsSubscription {
    std::string key;
    std::string subscribe_payload;
    std::string unsubscribe_payload;
    bool active = false;
};

struct WsConfig {
    int64_t ping_interval_ns = 15'000'000'000LL;
    int64_t stale_timeout_ns = 45'000'000'000LL;
    int64_t reconnect_base_delay_ms = 500;
    int64_t reconnect_max_delay_ms = 30'000;
    size_t max_message_bytes = 16 * 1024 * 1024;
    size_t max_outbound_queue = 8192;
    bool auto_pong = true;
    bool auto_resubscribe = true;
};

struct WsStats {
    int64_t messages_in = 0;
    int64_t messages_out = 0;
    int64_t bytes_in = 0;
    int64_t bytes_out = 0;
    int64_t pings_in = 0;
    int64_t pings_out = 0;
    int64_t pongs_in = 0;
    int64_t pongs_out = 0;
    int64_t reconnects = 0;
    int64_t errors = 0;
    int64_t stale_events = 0;
    int64_t dropped_outbound = 0;
    double avg_rx_latency_us = 0.0;
};

struct WsEvent {
    WsEventType type = WsEventType::Message;
    WsState state = WsState::Disconnected;
    std::string message;
    int64_t ts = 0;
};

using WsMessageCallback = std::function<void(const WsMessage&)>;
using WsStateCallback = std::function<void(WsState)>;
using WsErrorCallback = std::function<void(const std::string&)>;
using WsEventCallback = std::function<void(const WsEvent&)>;

class IWebSocketTransport {
public:
    virtual ~IWebSocketTransport() = default;

    virtual bool connect(const std::string& url) = 0;
    virtual void disconnect() = 0;
    virtual bool send(const WsMessage& msg) = 0;
    virtual bool poll(WsMessage& out) = 0;
    virtual WsState state() const = 0;
};

class MockWebSocketTransport final : public IWebSocketTransport {
public:
    bool connect(const std::string& url) override {
        url_ = url;
        state_ = WsState::Connected;
        return true;
    }

    void disconnect() override {
        state_ = WsState::Disconnected;
    }

    bool send(const WsMessage& msg) override {
        if (state_ != WsState::Connected) return false;
        sent_.push_back(msg);
        return true;
    }

    bool poll(WsMessage& out) override {
        if (incoming_.empty()) return false;
        out = std::move(incoming_.front());
        incoming_.pop_front();
        return true;
    }

    WsState state() const override {
        return state_;
    }

    void inject(WsMessage msg) {
        msg.recv_ts = msg.recv_ts == 0 ? now_ns() : msg.recv_ts;
        incoming_.push_back(std::move(msg));
    }

    void force_state(WsState s) {
        state_ = s;
    }

    const std::deque<WsMessage>& sent() const noexcept {
        return sent_;
    }

    const std::string& url() const noexcept {
        return url_;
    }

private:
    WsState state_ = WsState::Disconnected;
    std::string url_;
    std::deque<WsMessage> incoming_;
    std::deque<WsMessage> sent_;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

class WebSocketEngine {
public:
    explicit WebSocketEngine(IWebSocketTransport& transport, WsConfig cfg = {})
        : transport_(transport), cfg_(cfg) {}

    bool connect(std::string url) {
        url_ = std::move(url);
        set_state(WsState::Connecting, WsEventType::Reconnecting);

        bool ok = transport_.connect(url_);
        if (!ok) {
            ++stats_.errors;
            set_state(WsState::Error, WsEventType::Error, "connect_failed");
            emit_error("websocket_connect_failed");
            return false;
        }

        last_rx_ns_ = now_ns();
        last_tx_ns_ = last_rx_ns_;
        last_ping_ns_ = 0;
        reconnect_attempt_ = 0;

        set_state(WsState::Connected, WsEventType::Connected);

        flush_outbound();

        if (cfg_.auto_resubscribe) {
            for (auto& [key, sub] : subs_) {
                if (!sub.subscribe_payload.empty()) {
                    send_text(sub.subscribe_payload);
                    sub.active = true;
                }
            }
        }

        return true;
    }

    void disconnect() {
        set_state(WsState::Closing, WsEventType::Close);
        transport_.disconnect();
        set_state(WsState::Disconnected, WsEventType::Disconnected);
    }

    bool reconnect() {
        ++stats_.reconnects;
        ++reconnect_attempt_;
        set_state(WsState::Reconnecting, WsEventType::Reconnecting);

        transport_.disconnect();
        return connect(url_);
    }

    int64_t next_reconnect_delay_ms() const {
        int64_t base = cfg_.reconnect_base_delay_ms;
        int64_t maxd = cfg_.reconnect_max_delay_ms;
        int64_t delay = base;

        for (int i = 1; i < reconnect_attempt_; ++i) {
            if (delay >= maxd / 2) {
                delay = maxd;
                break;
            }
            delay *= 2;
        }

        return std::min(delay, maxd);
    }

    bool add_subscription(std::string key, std::string sub_payload, std::string unsub_payload = "") {
        if (key.empty()) return false;

        WsSubscription s;
        s.key = key;
        s.subscribe_payload = std::move(sub_payload);
        s.unsubscribe_payload = std::move(unsub_payload);
        s.active = false;

        subs_[key] = std::move(s);
        emit_event(WsEventType::SubscriptionAdded, key);

        if (state() == WsState::Connected) {
            bool ok = send_text(subs_[key].subscribe_payload);
            subs_[key].active = ok;
            return ok;
        }

        return true;
    }

    bool remove_subscription(const std::string& key) {
        auto it = subs_.find(key);
        if (it == subs_.end()) return false;

        if (state() == WsState::Connected && !it->second.unsubscribe_payload.empty())
            send_text(it->second.unsubscribe_payload);

        subs_.erase(it);
        emit_event(WsEventType::SubscriptionRemoved, key);
        return true;
    }

    bool send_text(const std::string& payload) {
        return send_or_queue({WsMessageType::Text, payload, now_ns()});
    }

    bool send_binary(const std::string& payload) {
        return send_or_queue({WsMessageType::Binary, payload, now_ns()});
    }

    bool send_ping(const std::string& payload = "") {
        ++stats_.pings_out;
        last_ping_ns_ = now_ns();
        return send_or_queue({WsMessageType::Ping, payload, last_ping_ns_});
    }

    bool send_pong(const std::string& payload = "") {
        ++stats_.pongs_out;
        return send_or_queue({WsMessageType::Pong, payload, now_ns()});
    }

    void poll_once() {
        maybe_heartbeat();
        check_stale();

        WsMessage msg;
        while (transport_.poll(msg)) {
            handle_message(msg);
        }

        flush_outbound();
    }

    WsState state() const {
        return transport_.state();
    }

    const WsStats& stats() const noexcept {
        return stats_;
    }

    const std::unordered_map<std::string, WsSubscription>& subscriptions() const noexcept {
        return subs_;
    }

    void set_message_callback(WsMessageCallback cb) {
        msg_cb_ = std::move(cb);
    }

    void set_state_callback(WsStateCallback cb) {
        state_cb_ = std::move(cb);
    }

    void set_error_callback(WsErrorCallback cb) {
        error_cb_ = std::move(cb);
    }

    void set_event_callback(WsEventCallback cb) {
        event_cb_ = std::move(cb);
    }

private:
    IWebSocketTransport& transport_;
    WsConfig cfg_;
    std::string url_;
    WsStats stats_;

    std::unordered_map<std::string, WsSubscription> subs_;
    std::deque<WsMessage> outbound_;

    WsMessageCallback msg_cb_;
    WsStateCallback state_cb_;
    WsErrorCallback error_cb_;
    WsEventCallback event_cb_;

    int64_t last_rx_ns_ = 0;
    int64_t last_tx_ns_ = 0;
    int64_t last_ping_ns_ = 0;
    int reconnect_attempt_ = 0;
    bool stale_emitted_ = false;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    bool send_or_queue(WsMessage msg) {
        if (msg.payload.size() > cfg_.max_message_bytes) {
            ++stats_.errors;
            emit_error("websocket_message_too_large");
            return false;
        }

        if (transport_.state() != WsState::Connected) {
            if (outbound_.size() >= cfg_.max_outbound_queue) {
                ++stats_.dropped_outbound;
                ++stats_.errors;
                emit_error("websocket_outbound_queue_full");
                return false;
            }
            outbound_.push_back(std::move(msg));
            return true;
        }

        return send_now(msg);
    }

    bool send_now(const WsMessage& msg) {
        bool ok = transport_.send(msg);

        if (!ok) {
            ++stats_.errors;
            emit_error("websocket_send_failed");
            return false;
        }

        ++stats_.messages_out;
        stats_.bytes_out += static_cast<int64_t>(msg.payload.size());
        last_tx_ns_ = now_ns();
        return true;
    }

    void flush_outbound() {
        if (transport_.state() != WsState::Connected) return;

        while (!outbound_.empty()) {
            WsMessage msg = std::move(outbound_.front());
            outbound_.pop_front();

            if (!send_now(msg)) break;
        }
    }

    void handle_message(const WsMessage& msg) {
        if (msg.payload.size() > cfg_.max_message_bytes) {
            ++stats_.errors;
            emit_error("websocket_inbound_message_too_large");
            return;
        }

        ++stats_.messages_in;
        stats_.bytes_in += static_cast<int64_t>(msg.payload.size());

        int64_t now = now_ns();
        last_rx_ns_ = now;
        stale_emitted_ = false;

        if (msg.recv_ts > 0) {
            double us = static_cast<double>(now - msg.recv_ts) / 1000.0;
            stats_.avg_rx_latency_us = stats_.avg_rx_latency_us * 0.98 + us * 0.02;
        }

        switch (msg.type) {
            case WsMessageType::Ping:
                ++stats_.pings_in;
                emit_event(WsEventType::Ping, msg.payload);
                if (cfg_.auto_pong) send_pong(msg.payload);
                break;

            case WsMessageType::Pong:
                ++stats_.pongs_in;
                emit_event(WsEventType::Pong, msg.payload);
                break;

            case WsMessageType::Close:
                emit_event(WsEventType::Close, msg.payload);
                transport_.disconnect();
                set_state(WsState::Disconnected, WsEventType::Disconnected);
                break;

            case WsMessageType::Text:
            case WsMessageType::Binary:
                emit_event(WsEventType::Message, "");
                if (msg_cb_) msg_cb_(msg);
                break;
        }
    }

    void maybe_heartbeat() {
        if (transport_.state() != WsState::Connected) return;

        int64_t now = now_ns();
        if (last_ping_ns_ == 0 || now - last_ping_ns_ >= cfg_.ping_interval_ns) {
            send_ping();
        }
    }

    void check_stale() {
        if (transport_.state() != WsState::Connected) return;
        if (last_rx_ns_ == 0) return;

        int64_t now = now_ns();
        if (!stale_emitted_ && now - last_rx_ns_ > cfg_.stale_timeout_ns) {
            stale_emitted_ = true;
            ++stats_.stale_events;
            ++stats_.errors;
            emit_event(WsEventType::Stale, "stale_connection");
            emit_error("websocket_stale_connection");
        }
    }

    void set_state(WsState s, WsEventType ev, const std::string& msg = "") {
        if (state_cb_) state_cb_(s);
        emit_event(ev, msg);
    }

    void emit_event(WsEventType t, const std::string& msg) {
        if (!event_cb_) return;

        WsEvent ev;
        ev.type = t;
        ev.state = transport_.state();
        ev.message = msg;
        ev.ts = now_ns();
        event_cb_(ev);
    }

    void emit_error(const std::string& e) {
        emit_event(WsEventType::Error, e);
        if (error_cb_) error_cb_(e);
    }
};

} // namespace hft