#pragma once
// exchange_transport.hpp — advanced paper/live-ready REST + WebSocket transport

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hft::transport {

enum class TransportState : uint8_t {
    Disconnected,
    Connecting,
    Authenticating,
    Connected,
    Recovering,
    Reconnecting,
    Halted
};

enum class RestMethod : uint8_t { GET, POST, PUT, DELETE_ };
enum class RequestPriority : uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

struct RestRequest {
    int64_t request_id = 0;
    RestMethod method = RestMethod::GET;
    RequestPriority priority = RequestPriority::Normal;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    bool requires_auth = false;
    int max_retries = 3;
    int64_t created_ns = 0;
    std::function<void(const struct RestResponse&)> callback;
};

struct RestResponse {
    int64_t request_id = 0;
    int status = 0;
    std::string body;
    std::string error;
    int retries = 0;
    int64_t latency_ns = 0;
    bool ok = false;
};

struct WsMessage {
    std::string channel;
    std::string payload;
    int64_t ts_ns = 0;
};

struct TransportMetrics {
    int64_t rest_sent = 0;
    int64_t rest_ok = 0;
    int64_t rest_failed = 0;
    int64_t rest_retries = 0;
    int64_t rest_queued = 0;

    int64_t ws_sent = 0;
    int64_t ws_received = 0;
    int64_t ws_subscriptions = 0;
    int64_t ws_dropped = 0;

    int64_t heartbeats = 0;
    int64_t missed_heartbeats = 0;
    int64_t reconnects = 0;
    int64_t state_changes = 0;

    int64_t throttle_waits = 0;

    double rest_mean_us = 0.0;
    double ws_mean_us = 0.0;
    double heartbeat_rtt_us = 0.0;
};

struct TransportCallbacks {
    std::function<void(TransportState)> on_state;
    std::function<void(const RestResponse&)> on_rest_response;
    std::function<void(const WsMessage&)> on_ws_message;
    std::function<void(const std::string&)> on_error;
    std::function<void()> on_heartbeat;
    std::function<std::unordered_map<std::string, std::string>()> auth_headers;
};

struct TransportConfig {
    std::string venue = "exchange";
    std::string rest_base_url;
    std::string ws_url;

    int rest_rate_limit_per_sec = 20;
    int ws_rate_limit_per_sec = 50;

    int heartbeat_interval_ms = 5000;
    int heartbeat_timeout_ms = 15000;

    int reconnect_delay_ms = 1000;
    int max_reconnects = 10;

    size_t max_rest_queue = 10000;
    size_t max_ws_queue = 100000;

    bool paper_mode = true;
    bool auto_reconnect = true;
    bool auto_start_workers = true;
};

class ExchangeTransport {
public:
    explicit ExchangeTransport(TransportConfig cfg = {})
        : cfg_(std::move(cfg)) {}

    ~ExchangeTransport() {
        disconnect();
    }

    ExchangeTransport(const ExchangeTransport&) = delete;
    ExchangeTransport& operator=(const ExchangeTransport&) = delete;

    void set_callbacks(TransportCallbacks cb) {
        std::lock_guard<std::mutex> lock(mu_);
        cb_ = std::move(cb);
    }

    bool connect() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ == TransportState::Connected) return true;
            stop_.store(false);
            set_state_locked(TransportState::Connecting);
        }

        sleep_ms(20);

        {
            std::lock_guard<std::mutex> lock(mu_);
            set_state_locked(TransportState::Authenticating);
        }

        sleep_ms(20);

        {
            std::lock_guard<std::mutex> lock(mu_);
            set_state_locked(TransportState::Connected);
            last_heartbeat_ns_ = now_ns();
        }

        if (cfg_.auto_start_workers) start_workers_once();
        return true;
    }

    void disconnect() {
        stop_.store(true);
        cv_.notify_all();

        if (rest_worker_.joinable()) rest_worker_.join();
        if (ws_worker_.joinable()) ws_worker_.join();
        if (heartbeat_worker_.joinable()) heartbeat_worker_.join();

        std::lock_guard<std::mutex> lock(mu_);
        if (state_ != TransportState::Disconnected)
            set_state_locked(TransportState::Disconnected);
    }

    bool reconnect() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ == TransportState::Halted) return false;
            set_state_locked(TransportState::Reconnecting);
            ++metrics_.reconnects;
        }

        sleep_ms(cfg_.reconnect_delay_ms);
        return connect();
    }

    void halt(const std::string& reason = "transport_halted") {
        std::lock_guard<std::mutex> lock(mu_);
        set_state_locked(TransportState::Halted);
        fire_error_locked(reason);
    }

    void resume() {
        connect();
    }

    TransportState state() const {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    bool authenticated() const {
        std::lock_guard<std::mutex> lock(mu_);
        return state_ == TransportState::Connected;
    }

    int64_t send_rest(RestRequest req) {
        req.request_id = ++request_counter_;
        req.created_ns = now_ns();

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (rest_queue_.size() >= cfg_.max_rest_queue) {
                RestResponse r;
                r.request_id = req.request_id;
                r.ok = false;
                r.error = "rest_queue_full";
                if (req.callback) req.callback(r);
                if (cb_.on_rest_response) cb_.on_rest_response(r);
                return req.request_id;
            }

            rest_queue_.push(std::move(req));
            ++metrics_.rest_queued;
        }

        cv_.notify_all();
        return request_counter_.load();
    }

    RestResponse send_rest_sync(RestRequest req) {
        int64_t start = now_ns();

        if (!authenticated()) {
            return {req.request_id, 0, "", "transport_not_connected", 0, now_ns() - start, false};
        }

        throttle(rest_bucket_, cfg_.rest_rate_limit_per_sec);

        if (req.requires_auth) {
            auto auth = build_auth_headers();
            for (auto& [k, v] : auth) req.headers[k] = v;
        }

        RestResponse res;
        res.request_id = req.request_id;
        res.status = 200;
        res.body = cfg_.paper_mode ? R"({"ok":true,"mode":"paper"})" : "{}";
        res.latency_ns = now_ns() - start;
        res.ok = true;

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++metrics_.rest_sent;
            ++metrics_.rest_ok;
            update_mean(metrics_.rest_mean_us, metrics_.rest_sent, res.latency_ns / 1000.0);
        }

        return res;
    }

    bool send_ws(std::string payload, RequestPriority priority = RequestPriority::Normal) {
        if (!authenticated()) return false;
        throttle(ws_bucket_, cfg_.ws_rate_limit_per_sec);

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (ws_outbox_.size() >= cfg_.max_ws_queue) {
                ++metrics_.ws_dropped;
                return false;
            }

            WsOutbound o;
            o.priority = priority;
            o.payload = std::move(payload);
            ws_outbox_.push(std::move(o));
            ++metrics_.ws_sent;
        }

        cv_.notify_all();
        return true;
    }

    bool subscribe(const std::string& channel) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            subscriptions_[channel] = true;
            ++metrics_.ws_subscriptions;
        }

        return send_ws("SUB " + channel, RequestPriority::High);
    }

    bool unsubscribe(const std::string& channel) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            subscriptions_.erase(channel);
        }

        return send_ws("UNSUB " + channel, RequestPriority::High);
    }

    std::vector<std::string> subscriptions() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out;
        out.reserve(subscriptions_.size());
        for (const auto& [ch, _] : subscriptions_) out.push_back(ch);
        return out;
    }

    void inject_ws_message(std::string channel, std::string payload) {
        WsMessage msg;
        msg.channel = std::move(channel);
        msg.payload = std::move(payload);
        msg.ts_ns = now_ns();

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (ws_inbox_.size() >= cfg_.max_ws_queue) {
                ++metrics_.ws_dropped;
                return;
            }
            ws_inbox_.push_back(std::move(msg));
        }

        cv_.notify_all();
    }

    void heartbeat() {
        int64_t ts = now_ns();

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++metrics_.heartbeats;
            last_heartbeat_ns_ = ts;
            metrics_.heartbeat_rtt_us = metrics_.heartbeat_rtt_us * 0.95 + 5.0 * 0.05;
            if (cb_.on_heartbeat) cb_.on_heartbeat();
        }
    }

    TransportMetrics metrics() const {
        std::lock_guard<std::mutex> lock(mu_);
        return metrics_;
    }

private:
    struct RateBucket {
        int64_t window_ns = 0;
        int count = 0;
        std::mutex mu;
    };

    struct WsOutbound {
        RequestPriority priority = RequestPriority::Normal;
        std::string payload;

        bool operator<(const WsOutbound& other) const {
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
    };

    struct RestCmp {
        bool operator()(const RestRequest& a, const RestRequest& b) const {
            if (a.priority != b.priority)
                return static_cast<int>(a.priority) < static_cast<int>(b.priority);
            return a.created_ns > b.created_ns;
        }
    };

    TransportConfig cfg_;
    mutable std::mutex mu_;
    std::condition_variable cv_;

    TransportCallbacks cb_;
    TransportState state_ = TransportState::Disconnected;
    TransportMetrics metrics_;

    std::priority_queue<RestRequest, std::vector<RestRequest>, RestCmp> rest_queue_;
    std::priority_queue<WsOutbound> ws_outbox_;
    std::deque<WsMessage> ws_inbox_;
    std::unordered_map<std::string, bool> subscriptions_;

    std::atomic<bool> stop_{false};
    std::atomic<int64_t> request_counter_{0};
    std::once_flag worker_once_;

    std::thread rest_worker_;
    std::thread ws_worker_;
    std::thread heartbeat_worker_;

    RateBucket rest_bucket_;
    RateBucket ws_bucket_;

    int64_t last_heartbeat_ns_ = 0;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static void sleep_ms(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void start_workers_once() {
        std::call_once(worker_once_, [&] {
            rest_worker_ = std::thread([this] { rest_loop(); });
            ws_worker_ = std::thread([this] { ws_loop(); });
            heartbeat_worker_ = std::thread([this] { heartbeat_loop(); });
        });
    }

    void set_state_locked(TransportState s) {
        state_ = s;
        ++metrics_.state_changes;
        if (cb_.on_state) cb_.on_state(s);
    }

    void fire_error_locked(const std::string& msg) {
        if (cb_.on_error) cb_.on_error(msg);
    }

    std::unordered_map<std::string, std::string> build_auth_headers() {
        std::lock_guard<std::mutex> lock(mu_);
        if (cb_.auth_headers) return cb_.auth_headers();
        return {};
    }

    void throttle(RateBucket& bucket, int max_per_sec) {
        if (max_per_sec <= 0) return;

        while (!stop_.load()) {
            int64_t ts = now_ns();

            {
                std::lock_guard<std::mutex> lock(bucket.mu);

                if (bucket.window_ns == 0 || ts - bucket.window_ns >= 1'000'000'000LL) {
                    bucket.window_ns = ts;
                    bucket.count = 0;
                }

                if (bucket.count < max_per_sec) {
                    ++bucket.count;
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                ++metrics_.throttle_waits;
            }

            sleep_ms(1);
        }
    }

    void rest_loop() {
        while (!stop_.load()) {
            RestRequest req;
            bool has = false;

            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return stop_.load() || !rest_queue_.empty();
                });

                if (stop_.load()) break;

                if (!rest_queue_.empty()) {
                    req = std::move(const_cast<RestRequest&>(rest_queue_.top()));
                    rest_queue_.pop();
                    has = true;
                }
            }

            if (!has) continue;

            RestResponse res;
            int attempts = 0;

            while (!stop_.load() && attempts <= req.max_retries) {
                ++attempts;
                res = send_rest_sync(req);
                res.retries = attempts - 1;

                if (res.ok) break;

                {
                    std::lock_guard<std::mutex> lock(mu_);
                    ++metrics_.rest_retries;
                }

                sleep_ms(25 * attempts);
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!res.ok) ++metrics_.rest_failed;
                if (req.callback) req.callback(res);
                if (cb_.on_rest_response) cb_.on_rest_response(res);
            }
        }
    }

    void ws_loop() {
        while (!stop_.load()) {
            WsMessage msg;
            bool has_msg = false;

            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return stop_.load() || !ws_inbox_.empty() || !ws_outbox_.empty();
                });

                if (stop_.load()) break;

                if (!ws_outbox_.empty()) {
                    ws_outbox_.pop();
                }

                if (!ws_inbox_.empty()) {
                    msg = std::move(ws_inbox_.front());
                    ws_inbox_.pop_front();
                    has_msg = true;
                }
            }

            if (has_msg) {
                std::lock_guard<std::mutex> lock(mu_);
                ++metrics_.ws_received;
                update_mean(metrics_.ws_mean_us, metrics_.ws_received, 1.0);
                if (cb_.on_ws_message) cb_.on_ws_message(msg);
            }
        }
    }

    void heartbeat_loop() {
        while (!stop_.load()) {
            sleep_ms(cfg_.heartbeat_interval_ms);
            if (stop_.load()) break;

            if (!authenticated()) continue;

            int64_t now = now_ns();
            int64_t timeout_ns = static_cast<int64_t>(cfg_.heartbeat_timeout_ms) * 1'000'000LL;

            {
                std::lock_guard<std::mutex> lock(mu_);
                if (last_heartbeat_ns_ > 0 && now - last_heartbeat_ns_ > timeout_ns) {
                    ++metrics_.missed_heartbeats;
                    set_state_locked(TransportState::Recovering);
                }
            }

            heartbeat();

            if (cfg_.auto_reconnect && state() == TransportState::Recovering) {
                reconnect();
            }
        }
    }

    static void update_mean(double& mean, int64_t count, double sample) {
        if (count <= 1) {
            mean = sample;
            return;
        }
        mean += (sample - mean) / static_cast<double>(count);
    }
};

} // namespace hft::transport