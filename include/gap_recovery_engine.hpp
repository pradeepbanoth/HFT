#pragma once

#include "market_data_engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::marketdata {

enum class RecoveryState : uint8_t {
    Synced,
    GapDetected,
    SnapshotRequested,
    ReplayingBuffered,
    Failed
};

enum class RecoveryDecision : uint8_t {
    ApplyNow,
    Buffered,
    Duplicate,
    Old,
    GapDetected,
    BufferFull,
    RecoveryTimedOut
};

struct GapRecoveryConfig {
    std::size_t max_buffered_packets_per_stream{4096};
    uint64_t recovery_timeout_ns{5'000'000'000ULL};
    bool request_snapshot_on_gap{true};
    bool allow_buffer_replay{true};
};

struct RecoveryMetrics {
    std::atomic<uint64_t> packets_seen{0};
    std::atomic<uint64_t> packets_apply_now{0};
    std::atomic<uint64_t> gaps_detected{0};
    std::atomic<uint64_t> packets_buffered{0};
    std::atomic<uint64_t> packets_replayed{0};
    std::atomic<uint64_t> snapshots_requested{0};
    std::atomic<uint64_t> snapshots_applied{0};
    std::atomic<uint64_t> duplicates_dropped{0};
    std::atomic<uint64_t> old_packets_dropped{0};
    std::atomic<uint64_t> buffer_full_drops{0};
    std::atomic<uint64_t> recovery_timeouts{0};
};

struct RecoveryMetricsSnapshot {
    uint64_t packets_seen{0};
    uint64_t packets_apply_now{0};
    uint64_t gaps_detected{0};
    uint64_t packets_buffered{0};
    uint64_t packets_replayed{0};
    uint64_t snapshots_requested{0};
    uint64_t snapshots_applied{0};
    uint64_t duplicates_dropped{0};
    uint64_t old_packets_dropped{0};
    uint64_t buffer_full_drops{0};
    uint64_t recovery_timeouts{0};
};

struct RecoveryResult {
    RecoveryDecision decision{RecoveryDecision::ApplyNow};
    bool should_apply{false};
    bool should_request_snapshot{false};
    uint64_t expected_sequence{0};
    uint64_t received_sequence{0};
};

struct ReplayBatch {
    std::vector<RawMarketPacket> packets;

    bool empty() const noexcept {
        return packets.empty();
    }

    std::size_t size() const noexcept {
        return packets.size();
    }
};

inline uint64_t recovery_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

class GapRecoveryEngine {
public:
    using SnapshotRequestCallback =
        std::function<void(const std::string& key, uint64_t expected_sequence, uint64_t received_sequence)>;

    explicit GapRecoveryEngine(GapRecoveryConfig config = {})
        : config_(config) {}

    void set_snapshot_request_callback(SnapshotRequestCallback cb) {
        std::scoped_lock lock(mutex_);
        snapshot_cb_ = std::move(cb);
    }

    RecoveryResult on_packet(const std::string& key, const RawMarketPacket& packet) {
        std::scoped_lock lock(mutex_);

        metrics_.packets_seen.fetch_add(1, std::memory_order_relaxed);

        auto& stream = streams_[key];
        const uint64_t seq = packet.sequence;

        RecoveryResult result;
        result.received_sequence = seq;
        result.expected_sequence = stream.last_sequence == 0 ? seq : stream.last_sequence + 1;

        if (stream.state != RecoveryState::Synced && recovery_timed_out(stream)) {
            stream.state = RecoveryState::Failed;
            metrics_.recovery_timeouts.fetch_add(1, std::memory_order_relaxed);

            result.decision = RecoveryDecision::RecoveryTimedOut;
            return result;
        }

        if (stream.last_sequence == 0) {
            stream.last_sequence = seq;
            stream.state = RecoveryState::Synced;

            metrics_.packets_apply_now.fetch_add(1, std::memory_order_relaxed);

            result.decision = RecoveryDecision::ApplyNow;
            result.should_apply = true;
            return result;
        }

        if (seq == stream.last_sequence || stream.applied_sequences.contains(seq)) {
            metrics_.duplicates_dropped.fetch_add(1, std::memory_order_relaxed);
            result.decision = RecoveryDecision::Duplicate;
            return result;
        }

        if (seq < stream.last_sequence) {
            metrics_.old_packets_dropped.fetch_add(1, std::memory_order_relaxed);
            result.decision = RecoveryDecision::Old;
            return result;
        }

        if (seq == stream.last_sequence + 1 && stream.state == RecoveryState::Synced) {
            mark_applied(stream, seq);

            metrics_.packets_apply_now.fetch_add(1, std::memory_order_relaxed);

            result.decision = RecoveryDecision::ApplyNow;
            result.should_apply = true;
            return result;
        }

        if (stream.buffered.contains(seq)) {
            metrics_.duplicates_dropped.fetch_add(1, std::memory_order_relaxed);
            result.decision = RecoveryDecision::Duplicate;
            return result;
        }

        if (stream.buffered.size() >= config_.max_buffered_packets_per_stream) {
            stream.state = RecoveryState::Failed;
            metrics_.buffer_full_drops.fetch_add(1, std::memory_order_relaxed);
            result.decision = RecoveryDecision::BufferFull;
            return result;
        }

        stream.buffered[seq] = packet;
        stream.state = RecoveryState::GapDetected;

        if (stream.gap_detected_ns == 0) {
            stream.gap_detected_ns = recovery_now_ns();
        }

        metrics_.gaps_detected.fetch_add(1, std::memory_order_relaxed);
        metrics_.packets_buffered.fetch_add(1, std::memory_order_relaxed);

        result.decision = RecoveryDecision::GapDetected;
        result.should_request_snapshot = config_.request_snapshot_on_gap;

        if (config_.request_snapshot_on_gap) {
            stream.state = RecoveryState::SnapshotRequested;
            metrics_.snapshots_requested.fetch_add(1, std::memory_order_relaxed);

            if (snapshot_cb_) {
                snapshot_cb_(key, stream.last_sequence + 1, seq);
            }
        }

        return result;
    }

    ReplayBatch apply_snapshot(const std::string& key, uint64_t snapshot_sequence) {
        std::scoped_lock lock(mutex_);

        ReplayBatch batch;
        auto& stream = streams_[key];

        stream.last_sequence = snapshot_sequence;
        stream.state = RecoveryState::ReplayingBuffered;
        stream.gap_detected_ns = 0;

        metrics_.snapshots_applied.fetch_add(1, std::memory_order_relaxed);

        if (!config_.allow_buffer_replay) {
            stream.buffered.clear();
            stream.state = RecoveryState::Synced;
            return batch;
        }

        while (true) {
            const uint64_t next_seq = stream.last_sequence + 1;
            auto it = stream.buffered.find(next_seq);

            if (it == stream.buffered.end()) break;

            batch.packets.push_back(it->second);
            mark_applied(stream, next_seq);
            stream.buffered.erase(it);

            metrics_.packets_replayed.fetch_add(1, std::memory_order_relaxed);
        }

        erase_old_buffered_locked(stream);
        stream.state = RecoveryState::Synced;

        return batch;
    }

    bool snapshot_required(const std::string& key) const {
        std::scoped_lock lock(mutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) return false;

        return it->second.state == RecoveryState::GapDetected ||
               it->second.state == RecoveryState::SnapshotRequested ||
               it->second.state == RecoveryState::Failed;
    }

    RecoveryState state(const std::string& key) const {
        std::scoped_lock lock(mutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) return RecoveryState::Synced;

        return it->second.state;
    }

    uint64_t last_sequence(const std::string& key) const {
        std::scoped_lock lock(mutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) return 0;

        return it->second.last_sequence;
    }

    std::size_t buffered_count(const std::string& key) const {
        std::scoped_lock lock(mutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) return 0;

        return it->second.buffered.size();
    }

    void reset_stream(const std::string& key) {
        std::scoped_lock lock(mutex_);
        streams_.erase(key);
    }

    RecoveryMetricsSnapshot metrics_snapshot() const noexcept {
        return {
            metrics_.packets_seen.load(std::memory_order_relaxed),
            metrics_.packets_apply_now.load(std::memory_order_relaxed),
            metrics_.gaps_detected.load(std::memory_order_relaxed),
            metrics_.packets_buffered.load(std::memory_order_relaxed),
            metrics_.packets_replayed.load(std::memory_order_relaxed),
            metrics_.snapshots_requested.load(std::memory_order_relaxed),
            metrics_.snapshots_applied.load(std::memory_order_relaxed),
            metrics_.duplicates_dropped.load(std::memory_order_relaxed),
            metrics_.old_packets_dropped.load(std::memory_order_relaxed),
            metrics_.buffer_full_drops.load(std::memory_order_relaxed),
            metrics_.recovery_timeouts.load(std::memory_order_relaxed)
        };
    }

private:
    struct StreamRecovery {
        uint64_t last_sequence{0};
        RecoveryState state{RecoveryState::Synced};
        uint64_t gap_detected_ns{0};
        std::map<uint64_t, RawMarketPacket> buffered;
        std::map<uint64_t, bool> applied_sequences;
    };

    bool recovery_timed_out(const StreamRecovery& stream) const noexcept {
        if (stream.gap_detected_ns == 0) return false;
        return recovery_now_ns() - stream.gap_detected_ns > config_.recovery_timeout_ns;
    }

    void mark_applied(StreamRecovery& stream, uint64_t seq) {
        stream.last_sequence = seq;
        stream.applied_sequences[seq] = true;

        while (stream.applied_sequences.size() > 4096) {
            stream.applied_sequences.erase(stream.applied_sequences.begin());
        }
    }

    void erase_old_buffered_locked(StreamRecovery& stream) {
        auto it = stream.buffered.begin();

        while (it != stream.buffered.end()) {
            if (it->first <= stream.last_sequence) {
                it = stream.buffered.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    GapRecoveryConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamRecovery> streams_;
    SnapshotRequestCallback snapshot_cb_;
    RecoveryMetrics metrics_{};
};

} // namespace hft::marketdata