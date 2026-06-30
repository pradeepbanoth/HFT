#pragma once
// rest_client.hpp — advanced REST client abstraction with retries, backoff,
// circuit breaker, signing hooks, per-endpoint rate limits, telemetry, and mocks.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft {

enum class HttpMethod : uint8_t {
    GET,
    POST,
    PUT,
    PATCH,
    DELETE_
};

enum class RestErrorCode : uint8_t {
    None,
    NetworkError,
    Timeout,
    RateLimited,
    Unauthorized,
    Forbidden,
    NotFound,
    BadRequest,
    Conflict,
    ServerError,
    CircuitOpen,
    ParseError,
    InternalError
};

enum class RetryPolicy : uint8_t {
    None,
    SafeOnly,
    AllIdempotent
};

struct RestRequest {
    HttpMethod method = HttpMethod::GET;
    std::string path;

    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;

    std::string body;

    bool signed_request = false;
    bool idempotent = true;

    int64_t timeout_ms = 5000;
    int max_attempts = 3;

    std::string endpoint_key;
    std::string request_id;
    std::string idempotency_key;
};

struct RestResponse {
    int status = 0;
    RestErrorCode error = RestErrorCode::None;

    std::map<std::string, std::string> headers;
    std::string body;

    int64_t latency_us = 0;
    int attempts = 0;

    std::string request_id;
    std::string error_message;

    bool ok() const noexcept {
        return error == RestErrorCode::None && status >= 200 && status < 300;
    }
};

struct RestRateLimit {
    int64_t requests_per_second = 20;
    int64_t burst = 20;
};

struct RestRetryConfig {
    RetryPolicy policy = RetryPolicy::SafeOnly;
    int max_attempts = 3;
    int64_t base_backoff_ms = 25;
    int64_t max_backoff_ms = 1000;
    double jitter_frac = 0.25;
};

struct CircuitBreakerConfig {
    int64_t failure_threshold = 8;
    int64_t half_open_after_ms = 2000;
    double failure_rate_threshold = 0.50;
    int64_t rolling_window = 50;
};

struct RestClientConfig {
    RestRateLimit default_rate_limit;
    RestRetryConfig retry;
    CircuitBreakerConfig circuit;
    bool add_request_id_header = true;
    bool add_idempotency_header = true;
    bool verbose = false;
};

struct RestClientStats {
    int64_t requests = 0;
    int64_t transport_calls = 0;
    int64_t success = 0;
    int64_t failed = 0;
    int64_t retries = 0;
    int64_t rate_limited = 0;
    int64_t circuit_rejected = 0;
    double avg_latency_us = 0.0;
};

inline std::string http_method_to_string(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::DELETE_: return "DELETE";
        default: return "UNKNOWN";
    }
}

inline std::string encode_query(const std::map<std::string, std::string>& q) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& [k, v] : q) {
        if (!first) oss << "&";
        first = false;
        oss << k << "=" << v;
    }
    return oss.str();
}

inline RestErrorCode classify_http_status(int status) {
    if (status >= 200 && status < 300) return RestErrorCode::None;
    if (status == 400) return RestErrorCode::BadRequest;
    if (status == 401) return RestErrorCode::Unauthorized;
    if (status == 403) return RestErrorCode::Forbidden;
    if (status == 404) return RestErrorCode::NotFound;
    if (status == 409) return RestErrorCode::Conflict;
    if (status == 429) return RestErrorCode::RateLimited;
    if (status >= 500) return RestErrorCode::ServerError;
    return RestErrorCode::InternalError;
}

class TokenBucketLimiter {
public:
    explicit TokenBucketLimiter(RestRateLimit limit = {})
        : limit_(limit)
        , tokens_(static_cast<double>(std::max<int64_t>(1, limit.burst)))
        , last_ns_(now_ns())
    {}

    void set_limit(RestRateLimit limit) {
        refill();
        limit_ = limit;
        tokens_ = std::min(tokens_, static_cast<double>(std::max<int64_t>(1, limit_.burst)));
    }

    bool allow() {
        refill();
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            return true;
        }
        return false;
    }

    void wait() {
        while (!allow()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    RestRateLimit limit_;
    double tokens_ = 0.0;
    int64_t last_ns_ = 0;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    void refill() {
        int64_t n = now_ns();
        double elapsed = static_cast<double>(n - last_ns_) / 1e9;
        last_ns_ = n;

        tokens_ += elapsed * static_cast<double>(std::max<int64_t>(1, limit_.requests_per_second));
        tokens_ = std::min(tokens_, static_cast<double>(std::max<int64_t>(1, limit_.burst)));
    }
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(CircuitBreakerConfig cfg = {})
        : cfg_(cfg) {}

    bool allow() {
        int64_t now = now_ms();

        if (open_) {
            if (now - opened_at_ms_ >= cfg_.half_open_after_ms) {
                half_open_ = true;
                return true;
            }
            return false;
        }

        return true;
    }

    void record(bool ok) {
        outcomes_.push_back(ok);
        if (static_cast<int64_t>(outcomes_.size()) > cfg_.rolling_window)
            outcomes_.pop_front();

        if (ok) {
            if (half_open_) {
                open_ = false;
                half_open_ = false;
            }
            consecutive_failures_ = 0;
            return;
        }

        ++consecutive_failures_;

        int64_t failures = 0;
        for (bool x : outcomes_) {
            if (!x) ++failures;
        }

        double rate = outcomes_.empty()
            ? 0.0
            : static_cast<double>(failures) / static_cast<double>(outcomes_.size());

        if (consecutive_failures_ >= cfg_.failure_threshold ||
            (static_cast<int64_t>(outcomes_.size()) >= cfg_.rolling_window && rate >= cfg_.failure_rate_threshold)) {
            open_ = true;
            half_open_ = false;
            opened_at_ms_ = now_ms();
        }
    }

    bool is_open() const noexcept { return open_; }

private:
    CircuitBreakerConfig cfg_;
    std::deque<bool> outcomes_;
    int64_t consecutive_failures_ = 0;
    bool open_ = false;
    bool half_open_ = false;
    int64_t opened_at_ms_ = 0;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
};

class IRestTransport {
public:
    virtual ~IRestTransport() = default;
    virtual RestResponse send(const std::string& base_url, const RestRequest& req) = 0;
};

class MockRestTransport final : public IRestTransport {
public:
    using Handler = std::function<RestResponse(const std::string&, const RestRequest&)>;

    void set_handler(Handler h) {
        handler_ = std::move(h);
    }

    RestResponse send(const std::string& base_url, const RestRequest& req) override {
        if (handler_) return handler_(base_url, req);

        RestResponse r;
        r.status = 200;
        r.error = RestErrorCode::None;
        r.body = "{}";
        r.request_id = req.request_id;
        return r;
    }

private:
    Handler handler_;
};

class RestSigner {
public:
    using SignFn = std::function<void(RestRequest&)>;

    RestSigner() = default;
    explicit RestSigner(SignFn fn) : fn_(std::move(fn)) {}

    void sign(RestRequest& req) const {
        if (fn_) fn_(req);
    }

private:
    SignFn fn_;
};

class RestClient {
public:
    RestClient(
        std::string base_url,
        IRestTransport& transport,
        RestClientConfig cfg = {},
        RestSigner signer = {}
    )
        : base_url_(std::move(base_url))
        , transport_(transport)
        , cfg_(std::move(cfg))
        , default_limiter_(cfg_.default_rate_limit)
        , circuit_(cfg_.circuit)
        , signer_(std::move(signer))
    {}

    void set_endpoint_limit(const std::string& endpoint_key, RestRateLimit limit) {
        endpoint_limiters_.insert_or_assign(endpoint_key, TokenBucketLimiter(limit));
    }

    RestResponse send(RestRequest req) {
        ++stats_.requests;

        normalize(req);

        if (!circuit_.allow()) {
            ++stats_.circuit_rejected;
            RestResponse r;
            r.status = 0;
            r.error = RestErrorCode::CircuitOpen;
            r.request_id = req.request_id;
            r.error_message = "circuit_open";
            return r;
        }

        if (req.signed_request)
            signer_.sign(req);

        int attempts = std::max(1, std::min(req.max_attempts, cfg_.retry.max_attempts));
        RestResponse last;

        for (int attempt = 1; attempt <= attempts; ++attempt) {
            apply_rate_limit(req);

            auto t0 = std::chrono::high_resolution_clock::now();
            ++stats_.transport_calls;
            last = transport_.send(base_url_, req);
            auto t1 = std::chrono::high_resolution_clock::now();

            last.attempts = attempt;
            last.request_id = req.request_id;
            last.latency_us = static_cast<int64_t>(
                std::chrono::duration<double, std::micro>(t1 - t0).count()
            );

            if (last.error == RestErrorCode::None && last.status != 0)
                last.error = classify_http_status(last.status);

            stats_.avg_latency_us = stats_.avg_latency_us * 0.98 +
                                    static_cast<double>(last.latency_us) * 0.02;

            bool ok = last.ok();
            circuit_.record(ok);

            if (ok) {
                ++stats_.success;
                return last;
            }

            if (attempt < attempts && should_retry(req, last)) {
                ++stats_.retries;
                sleep_backoff(attempt);
                continue;
            }

            ++stats_.failed;
            return last;
        }

        ++stats_.failed;
        return last;
    }

    RestResponse get(std::string path, std::map<std::string, std::string> query = {}, bool sign = false) {
        RestRequest r;
        r.method = HttpMethod::GET;
        r.path = std::move(path);
        r.query = std::move(query);
        r.signed_request = sign;
        r.idempotent = true;
        return send(std::move(r));
    }

    RestResponse post(std::string path, std::string body = "{}", bool sign = true, bool idempotent = false) {
        RestRequest r;
        r.method = HttpMethod::POST;
        r.path = std::move(path);
        r.body = std::move(body);
        r.signed_request = sign;
        r.idempotent = idempotent;
        r.headers["Content-Type"] = "application/json";
        return send(std::move(r));
    }

    RestResponse put(std::string path, std::string body = "{}", bool sign = true) {
        RestRequest r;
        r.method = HttpMethod::PUT;
        r.path = std::move(path);
        r.body = std::move(body);
        r.signed_request = sign;
        r.idempotent = true;
        r.headers["Content-Type"] = "application/json";
        return send(std::move(r));
    }

    RestResponse patch(std::string path, std::string body = "{}", bool sign = true) {
        RestRequest r;
        r.method = HttpMethod::PATCH;
        r.path = std::move(path);
        r.body = std::move(body);
        r.signed_request = sign;
        r.idempotent = true;
        r.headers["Content-Type"] = "application/json";
        return send(std::move(r));
    }

    RestResponse del(std::string path, std::map<std::string, std::string> query = {}, bool sign = true) {
        RestRequest r;
        r.method = HttpMethod::DELETE_;
        r.path = std::move(path);
        r.query = std::move(query);
        r.signed_request = sign;
        r.idempotent = true;
        return send(std::move(r));
    }

    const RestClientStats& stats() const noexcept {
        return stats_;
    }

    bool circuit_open() const noexcept {
        return circuit_.is_open();
    }

private:
    std::string base_url_;
    IRestTransport& transport_;
    RestClientConfig cfg_;
    TokenBucketLimiter default_limiter_;
    std::unordered_map<std::string, TokenBucketLimiter> endpoint_limiters_;
    CircuitBreaker circuit_;
    RestSigner signer_;
    RestClientStats stats_;
    uint64_t request_seq_ = 0;

    void normalize(RestRequest& req) {
        if (req.endpoint_key.empty())
            req.endpoint_key = http_method_to_string(req.method) + " " + req.path;

        if (req.request_id.empty())
            req.request_id = "req_" + std::to_string(++request_seq_);

        if (req.idempotency_key.empty() && req.idempotent)
            req.idempotency_key = "idem_" + req.request_id;

        if (cfg_.add_request_id_header)
            req.headers["X-Request-Id"] = req.request_id;

        if (cfg_.add_idempotency_header && !req.idempotency_key.empty())
            req.headers["Idempotency-Key"] = req.idempotency_key;
    }

    void apply_rate_limit(const RestRequest& req) {
        auto it = endpoint_limiters_.find(req.endpoint_key);

        if (it != endpoint_limiters_.end()) {
            if (!it->second.allow()) {
                ++stats_.rate_limited;
                it->second.wait();
            }
            return;
        }

        if (!default_limiter_.allow()) {
            ++stats_.rate_limited;
            default_limiter_.wait();
        }
    }

    bool should_retry(const RestRequest& req, const RestResponse& res) const {
        if (cfg_.retry.policy == RetryPolicy::None)
            return false;

        if (cfg_.retry.policy == RetryPolicy::SafeOnly) {
            if (req.method != HttpMethod::GET && req.method != HttpMethod::DELETE_)
                return false;
        }

        if (cfg_.retry.policy == RetryPolicy::AllIdempotent && !req.idempotent)
            return false;

        return res.error == RestErrorCode::NetworkError ||
               res.error == RestErrorCode::Timeout ||
               res.error == RestErrorCode::RateLimited ||
               res.error == RestErrorCode::ServerError;
    }

    void sleep_backoff(int attempt) {
        int64_t base = cfg_.retry.base_backoff_ms;
        int64_t maxv = cfg_.retry.max_backoff_ms;

        int64_t delay = std::min<int64_t>(maxv, base * (1LL << std::min(attempt - 1, 10)));

        static thread_local std::mt19937_64 rng{1234567};
        double jf = std::clamp(cfg_.retry.jitter_frac, 0.0, 1.0);
        std::uniform_real_distribution<double> dist(1.0 - jf, 1.0 + jf);
        delay = static_cast<int64_t>(static_cast<double>(delay) * dist(rng));

        if (delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
};

} // namespace hft