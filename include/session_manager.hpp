#pragma once
// session_manager.hpp — advanced resilient exchange session manager

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class SessionState : uint8_t {
    Disconnected,
    Connecting,
    TcpConnected,
    TlsReady,
    Authenticating,
    Authenticated,
    Subscribing,
    Live,
    Stale,
    Reconnecting,
    Recovering,
    Halted
};

enum class SessionEventType : uint8_t {
    StateChanged,
    ConnectRequested,
    TcpConnected,
    TlsReady,
    AuthRequested,
    Authenticated,
    AuthExpired,
    Subscribed,
    Live,
    HeartbeatSent,
    HeartbeatReceived,
    StaleDetected,
    Disconnected,
    ReconnectScheduled,
    Recovered,
    SequenceGap,
    DuplicateSequence,
    OutOfOrderSequence,
    Halted,
    Error
};

inline const char* session_state_to_str(SessionState s) noexcept {
    switch (s) {
        case SessionState::Disconnected: return "disconnected";
        case SessionState::Connecting: return "connecting";
        case SessionState::TcpConnected: return "tcp_connected";
        case SessionState::TlsReady: return "tls_ready";
        case SessionState::Authenticating: return "authenticating";
        case SessionState::Authenticated: return "authenticated";
        case SessionState::Subscribing: return "subscribing";
        case SessionState::Live: return "live";
        case SessionState::Stale: return "stale";
        case SessionState::Reconnecting: return "reconnecting";
        case SessionState::Recovering: return "recovering";
        case SessionState::Halted: return "halted";
        default: return "unknown";
    }
}

struct SessionConfig {
    std::string venue = "unknown";

    int64_t heartbeat_interval_ns = 5'000'000'000LL;
    int64_t stale_timeout_ns = 15'000'000'000LL;
    int64_t reconnect_base_delay_ns = 1'000'000'000LL;
    int64_t reconnect_max_delay_ns = 30'000'000'000LL;
    int64_t auth_renew_before_ns = 60'000'000'000LL;

    int max_reconnects = 20;
    int event_history_limit = 512;

    double reconnect_jitter = 0.20;

    bool require_auth = false;
    bool require_tls = true;
    bool auto_reconnect = true;
};

struct SessionSubscription {
    std::string channel;
    std::string symbol;
    int depth = 0;

    std::string key() const {
        return channel + "|" + symbol + "|" + std::to_string(depth);
    }
};

struct SessionStats {
    int64_t connect_attempts = 0;
    int64_t reconnects = 0;
    int64_t successful_reconnects = 0;
    int64_t disconnects = 0;
    int64_t stale_events = 0;
    int64_t heartbeats_sent = 0;
    int64_t heartbeats_received = 0;
    int64_t subscriptions_sent = 0;
    int64_t subscriptions_restored = 0;
    int64_t auth_success = 0;
    int64_t auth_expired = 0;
    int64_t sequence_gaps = 0;
    int64_t duplicates = 0;
    int64_t out_of_order = 0;
    int64_t errors = 0;

    double avg_heartbeat_rtt_us = 0.0;
    double max_heartbeat_rtt_us = 0.0;
    double avg_reconnect_ms = 0.0;
};

struct SequenceState {
    int64_t last_seq = -1;
    int64_t expected_next = -1;
    int64_t gaps = 0;
    int64_t duplicates = 0;
    int64_t out_of_order = 0;

    SessionEventType update(int64_t seq) {
        if (seq <= 0) return SessionEventType::Live;

        if (last_seq < 0) {
            last_seq = seq;
            expected_next = seq + 1;
            return SessionEventType::Live;
        }

        if (seq == last_seq) {
            ++duplicates;
            return SessionEventType::DuplicateSequence;
        }

        if (seq < last_seq) {
            ++out_of_order;
            return SessionEventType::OutOfOrderSequence;
        }

        if (expected_next > 0 && seq != expected_next) {
            ++gaps;
            last_seq = seq;
            expected_next = seq + 1;
            return SessionEventType::SequenceGap;
        }

        last_seq = seq;
        expected_next = seq + 1;
        return SessionEventType::Live;
    }

    bool healthy() const noexcept {
        return gaps == 0 && out_of_order == 0;
    }
};

struct SessionEvent {
    SessionEventType type = SessionEventType::Live;
    SessionState state = SessionState::Disconnected;
    std::string venue;
    std::string message;
    int64_t ts = 0;
};

struct SessionHealth {
    SessionState state = SessionState::Disconnected;
    double score = 1.0;
    bool connected = false;
    bool stale = false;
    bool authenticated = false;
    int64_t reconnects = 0;
    int64_t sequence_gaps = 0;
    int64_t last_rx_ns = 0;
    int64_t last_heartbeat_ns = 0;
};

using SessionCallback = std::function<void(const SessionEvent&)>;

class SessionManager {
public:
    explicit SessionManager(SessionConfig cfg = {})
        : cfg_(std::move(cfg))
        , rng_(42)
    {}

    SessionState state() const noexcept { return state_; }
    const SessionStats& stats() const noexcept { return stats_; }
    const std::vector<SessionSubscription>& subscriptions() const noexcept { return subscriptions_; }
    const std::deque<SessionEvent>& event_history() const noexcept { return history_; }

    void set_callback(SessionCallback cb) { cb_ = std::move(cb); }

    SessionHealth health() const {
        SessionHealth h;
        h.state = state_;
        h.connected = state_ == SessionState::Live ||
                      state_ == SessionState::Authenticated ||
                      state_ == SessionState::Subscribing;
        h.stale = state_ == SessionState::Stale;
        h.authenticated = authenticated_;
        h.reconnects = stats_.reconnects;
        h.sequence_gaps = stats_.sequence_gaps;
        h.last_rx_ns = last_rx_ns_;
        h.last_heartbeat_ns = last_heartbeat_sent_ns_;

        double score = 1.0;
        score -= std::min(0.4, stats_.stale_events * 0.05);
        score -= std::min(0.3, stats_.sequence_gaps * 0.02);
        score -= std::min(0.2, stats_.reconnects * 0.01);
        score -= state_ == SessionState::Halted ? 1.0 : 0.0;
        h.score = std::clamp(score, 0.0, 1.0);
        return h;
    }

    SequenceState sequence(const std::string& stream) const {
        auto it = sequences_.find(stream);
        return it == sequences_.end() ? SequenceState{} : it->second;
    }

    void add_subscription(SessionSubscription sub) {
        const std::string k = sub.key();
        auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
            [&](const SessionSubscription& s) { return s.key() == k; });

        if (it == subscriptions_.end())
            subscriptions_.push_back(std::move(sub));
    }

    void clear_subscriptions() {
        subscriptions_.clear();
    }

    bool connect(int64_t now_ns) {
        if (state_ == SessionState::Halted) return false;

        ++stats_.connect_attempts;
        reconnect_attempt_ = 0;
        transition(SessionState::Connecting, SessionEventType::ConnectRequested,
                   "connect_requested", now_ns);
        return true;
    }

    bool on_tcp_connected(int64_t now_ns) {
        if (state_ != SessionState::Connecting &&
            state_ != SessionState::Reconnecting)
            return false;

        transition(SessionState::TcpConnected, SessionEventType::TcpConnected,
                   "tcp_connected", now_ns);

        if (!cfg_.require_tls)
            return on_tls_ready(now_ns);

        return true;
    }

    bool on_tls_ready(int64_t now_ns) {
        if (state_ != SessionState::TcpConnected)
            return false;

        transition(SessionState::TlsReady, SessionEventType::TlsReady,
                   "tls_ready", now_ns);

        if (!cfg_.require_auth)
            return on_authenticated(now_ns, 0);

        transition(SessionState::Authenticating, SessionEventType::AuthRequested,
                   "auth_requested", now_ns);
        return true;
    }

    bool on_authenticated(int64_t now_ns, int64_t expires_at_ns = 0) {
        if (state_ != SessionState::TlsReady &&
            state_ != SessionState::Authenticating)
            return false;

        authenticated_ = true;
        auth_expires_at_ns_ = expires_at_ns;
        ++stats_.auth_success;

        transition(SessionState::Authenticated, SessionEventType::Authenticated,
                   "authenticated", now_ns);

        return resubscribe(now_ns);
    }

    bool resubscribe(int64_t now_ns) {
        if (state_ != SessionState::Authenticated &&
            state_ != SessionState::Recovering)
            return false;

        transition(SessionState::Subscribing, SessionEventType::Subscribed,
                   "subscriptions_sent", now_ns);

        stats_.subscriptions_sent += static_cast<int64_t>(subscriptions_.size());
        if (stats_.reconnects > 0)
            stats_.subscriptions_restored += static_cast<int64_t>(subscriptions_.size());

        return on_live(now_ns);
    }

    bool on_live(int64_t now_ns) {
        last_rx_ns_ = now_ns;
        last_heartbeat_sent_ns_ = now_ns;
        transition(SessionState::Live, SessionEventType::Live, "live", now_ns);

        if (recovering_) {
            recovering_ = false;
            ++stats_.successful_reconnects;
            update_reconnect_latency(now_ns);
            emit(SessionEventType::Recovered, "session_recovered", now_ns);
        }

        return true;
    }

    void on_message_rx(int64_t now_ns) {
        last_rx_ns_ = now_ns;
        if (state_ == SessionState::Stale)
            on_live(now_ns);
    }

    SessionEventType on_sequence(const std::string& stream, int64_t seq, int64_t now_ns = 0) {
        auto type = sequences_[stream].update(seq);

        if (type == SessionEventType::SequenceGap) {
            ++stats_.sequence_gaps;
            emit(type, "sequence_gap:" + stream, now_ns);
        } else if (type == SessionEventType::DuplicateSequence) {
            ++stats_.duplicates;
            emit(type, "duplicate_sequence:" + stream, now_ns);
        } else if (type == SessionEventType::OutOfOrderSequence) {
            ++stats_.out_of_order;
            emit(type, "out_of_order_sequence:" + stream, now_ns);
        }

        return type;
    }

    bool should_send_heartbeat(int64_t now_ns) const {
        return state_ == SessionState::Live &&
               now_ns - last_heartbeat_sent_ns_ >= cfg_.heartbeat_interval_ns;
    }

    void on_heartbeat_sent(int64_t now_ns) {
        last_heartbeat_sent_ns_ = now_ns;
        pending_heartbeat_ns_ = now_ns;
        ++stats_.heartbeats_sent;
        emit(SessionEventType::HeartbeatSent, "heartbeat_sent", now_ns);
    }

    void on_heartbeat_received(int64_t now_ns) {
        ++stats_.heartbeats_received;
        last_rx_ns_ = now_ns;

        if (pending_heartbeat_ns_ > 0) {
            double rtt_us = static_cast<double>(now_ns - pending_heartbeat_ns_) / 1000.0;
            stats_.avg_heartbeat_rtt_us =
                stats_.avg_heartbeat_rtt_us * 0.95 + rtt_us * 0.05;
            stats_.max_heartbeat_rtt_us = std::max(stats_.max_heartbeat_rtt_us, rtt_us);
            pending_heartbeat_ns_ = 0;
        }

        emit(SessionEventType::HeartbeatReceived, "heartbeat_received", now_ns);
    }

    bool check_stale(int64_t now_ns) {
        if (state_ != SessionState::Live) return false;

        if (now_ns - last_rx_ns_ > cfg_.stale_timeout_ns) {
            ++stats_.stale_events;
            transition(SessionState::Stale, SessionEventType::StaleDetected,
                       "stale_session", now_ns);

            if (cfg_.auto_reconnect)
                schedule_reconnect(now_ns, "stale_timeout");

            return true;
        }

        return false;
    }

    bool check_auth_expiry(int64_t now_ns) {
        if (!cfg_.require_auth || auth_expires_at_ns_ <= 0) return false;

        if (now_ns + cfg_.auth_renew_before_ns >= auth_expires_at_ns_) {
            authenticated_ = false;
            ++stats_.auth_expired;
            transition(SessionState::Authenticating, SessionEventType::AuthExpired,
                       "auth_expiring", now_ns);
            return true;
        }

        return false;
    }

    void on_disconnect(int64_t now_ns, const std::string& reason = "disconnect") {
        ++stats_.disconnects;
        authenticated_ = false;
        transition(SessionState::Disconnected, SessionEventType::Disconnected, reason, now_ns);

        if (cfg_.auto_reconnect)
            schedule_reconnect(now_ns, reason);
    }

    bool schedule_reconnect(int64_t now_ns, const std::string& reason = "reconnect") {
        if (!cfg_.auto_reconnect) return false;

        if (reconnect_attempt_ >= cfg_.max_reconnects) {
            halt(now_ns, "max_reconnects_exceeded");
            return false;
        }

        ++reconnect_attempt_;
        ++stats_.reconnects;
        reconnect_started_ns_ = now_ns;
        recovering_ = true;
        next_reconnect_ns_ = now_ns + reconnect_delay_ns();

        transition(SessionState::Reconnecting, SessionEventType::ReconnectScheduled,
                   reason, now_ns);
        return true;
    }

    bool should_reconnect_now(int64_t now_ns) const {
        return state_ == SessionState::Reconnecting &&
               next_reconnect_ns_ > 0 &&
               now_ns >= next_reconnect_ns_;
    }

    bool begin_reconnect(int64_t now_ns) {
        if (!should_reconnect_now(now_ns)) return false;

        transition(SessionState::Connecting, SessionEventType::ConnectRequested,
                   "begin_reconnect", now_ns);
        return true;
    }

    void halt(int64_t now_ns, const std::string& reason = "halted") {
        ++stats_.errors;
        transition(SessionState::Halted, SessionEventType::Halted, reason, now_ns);
    }

    void reset(int64_t now_ns) {
        state_ = SessionState::Disconnected;
        authenticated_ = false;
        reconnect_attempt_ = 0;
        next_reconnect_ns_ = 0;
        recovering_ = false;
        last_rx_ns_ = now_ns;
        last_heartbeat_sent_ns_ = now_ns;
        pending_heartbeat_ns_ = 0;
        auth_expires_at_ns_ = 0;
        sequences_.clear();
    }

private:
    SessionConfig cfg_;
    SessionState state_ = SessionState::Disconnected;
    SessionStats stats_;
    std::vector<SessionSubscription> subscriptions_;
    std::unordered_map<std::string, SequenceState> sequences_;
    std::deque<SessionEvent> history_;
    SessionCallback cb_;

    std::mt19937_64 rng_;

    bool authenticated_ = false;
    bool recovering_ = false;

    int reconnect_attempt_ = 0;
    int64_t next_reconnect_ns_ = 0;
    int64_t reconnect_started_ns_ = 0;

    int64_t last_rx_ns_ = 0;
    int64_t last_heartbeat_sent_ns_ = 0;
    int64_t pending_heartbeat_ns_ = 0;
    int64_t auth_expires_at_ns_ = 0;

    void transition(SessionState next, SessionEventType type,
                    const std::string& msg, int64_t ts) {
        state_ = next;
        emit(SessionEventType::StateChanged, session_state_to_str(next), ts);
        emit(type, msg, ts);
    }

    void emit(SessionEventType type, const std::string& msg, int64_t ts) {
        SessionEvent ev;
        ev.type = type;
        ev.state = state_;
        ev.venue = cfg_.venue;
        ev.message = msg;
        ev.ts = ts;

        history_.push_back(ev);
        while (static_cast<int>(history_.size()) > cfg_.event_history_limit)
            history_.pop_front();

        if (cb_) cb_(ev);
    }

    int64_t reconnect_delay_ns() {
        int capped = std::min(reconnect_attempt_ - 1, 10);
        double base = static_cast<double>(cfg_.reconnect_base_delay_ns) *
                      static_cast<double>(1LL << capped);

        base = std::min(base, static_cast<double>(cfg_.reconnect_max_delay_ns));

        std::uniform_real_distribution<double> jitter(
            1.0 - cfg_.reconnect_jitter,
            1.0 + cfg_.reconnect_jitter
        );

        return static_cast<int64_t>(base * jitter(rng_));
    }

    void update_reconnect_latency(int64_t now_ns) {
        if (reconnect_started_ns_ <= 0) return;

        double ms = static_cast<double>(now_ns - reconnect_started_ns_) / 1e6;
        stats_.avg_reconnect_ms = stats_.avg_reconnect_ms * 0.90 + ms * 0.10;
        reconnect_started_ns_ = 0;
    }
};

} // namespace hft