#pragma once

#include "child_order_manager.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft::execution {

enum class ChildCommandType : uint8_t {
    Submit,
    Cancel,
    Replace,
    Retry
};

enum class ChildCommandState : uint8_t {
    Created,
    Sent,
    Acked,
    Failed,
    Expired
};

struct RetryPolicy {
    uint32_t max_retries{3};
    uint64_t retry_delay_ns{1'000'000};
};

struct ChildOrderCommand {
    uint64_t command_id{0};

    ChildCommandType type{ChildCommandType::Submit};
    ChildCommandState state{ChildCommandState::Created};

    uint64_t parent_id{0};
    uint64_t child_id{0};

    std::string venue;
    std::string symbol;
    ChildOrderSide side{ChildOrderSide::Buy};

    double quantity{0.0};
    double price{0.0};

    std::string client_order_id;
    std::string venue_order_id;
    std::string idempotency_key;

    uint64_t timestamp_ns{0};
    uint64_t create_time_ns{0};
    uint64_t sent_time_ns{0};
    uint64_t last_update_ns{0};

    std::string failure_reason;
};

struct CommandMetrics {
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> acked{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> expired{0};
    std::atomic<uint64_t> retries{0};
    std::atomic<uint64_t> duplicates_blocked{0};
};

class ChildOrderCommandFactory {
public:
    explicit ChildOrderCommandFactory(
        ChildOrderManager& manager,
        RetryPolicy retry_policy = {}
    )
        : manager_(manager), retry_policy_(retry_policy) {}

    std::vector<ChildOrderCommand> build_submit_commands(
        const std::vector<uint64_t>& child_ids,
        uint64_t timestamp_ns
    ) {
        std::vector<ChildOrderCommand> commands;

        for (auto child_id : child_ids) {
            auto child = manager_.child(child_id);
            if (!child.has_value()) continue;

            if (!manager_.mark_submitted(child_id, timestamp_ns)) continue;

            auto cmd = make_command(
                ChildCommandType::Submit,
                *child,
                child->quantity,
                child->limit_price,
                timestamp_ns
            );

            if (!register_command(cmd)) continue;
            commands.push_back(cmd);
        }

        return commands;
    }

    std::optional<ChildOrderCommand> build_cancel_command(
        uint64_t child_id,
        uint64_t timestamp_ns
    ) {
        auto child = manager_.child(child_id);
        if (!child.has_value()) return std::nullopt;

        if (!manager_.mark_cancel_pending(child_id, timestamp_ns)) return std::nullopt;

        auto cmd = make_command(
            ChildCommandType::Cancel,
            *child,
            child->remaining_quantity,
            child->limit_price,
            timestamp_ns
        );

        if (!register_command(cmd)) return std::nullopt;
        return cmd;
    }

    std::optional<ChildOrderCommand> build_replace_command(
        uint64_t child_id,
        double new_qty,
        double new_price,
        uint64_t timestamp_ns
    ) {
        auto child = manager_.child(child_id);
        if (!child.has_value()) return std::nullopt;
        if (new_qty <= 0.0 || new_price <= 0.0) return std::nullopt;

        if (!manager_.mark_replace_pending(child_id, timestamp_ns)) return std::nullopt;

        auto cmd = make_command(
            ChildCommandType::Replace,
            *child,
            new_qty,
            new_price,
            timestamp_ns
        );

        if (!register_command(cmd)) return std::nullopt;
        return cmd;
    }

    std::optional<ChildOrderCommand> build_retry_command(
        uint64_t failed_child_id,
        const std::vector<std::string>& fallback_venues,
        uint64_t timestamp_ns
    ) {
        auto failed = manager_.child(failed_child_id);
        if (!failed.has_value()) return std::nullopt;
        if (failed->remaining_quantity <= 0.0) return std::nullopt;
        if (failed->retry_count >= retry_policy_.max_retries) return std::nullopt;

        std::string venue = select_fallback_venue(*failed, fallback_venues);
        if (venue.empty()) return std::nullopt;

        if (!manager_.increment_retry(failed_child_id, timestamp_ns)) return std::nullopt;

        ParentCreateRequest retry_parent;
        retry_parent.symbol = failed->symbol;
        retry_parent.side = failed->side;
        retry_parent.quantity = failed->remaining_quantity;
        retry_parent.timestamp_ns = timestamp_ns;

        auto parent_id = manager_.create_parent(retry_parent);
        if (parent_id == 0) return std::nullopt;

        std::vector<ChildOrderRoute> routes = {
            {venue, failed->remaining_quantity, failed->limit_price}
        };

        auto retry_children = manager_.create_children(parent_id, routes, timestamp_ns);
        if (retry_children.empty()) return std::nullopt;

        auto retry_child = manager_.child(retry_children.front());
        if (!retry_child.has_value()) return std::nullopt;

        if (!manager_.mark_submitted(retry_child->child_id, timestamp_ns)) return std::nullopt;

        auto cmd = make_command(
            ChildCommandType::Retry,
            *retry_child,
            retry_child->quantity,
            retry_child->limit_price,
            timestamp_ns
        );

        if (!register_command(cmd)) return std::nullopt;

        metrics_.retries.fetch_add(1, std::memory_order_relaxed);
        return cmd;
    }

    bool mark_sent(uint64_t command_id, uint64_t timestamp_ns) {
        auto* cmd = find_command(command_id);
        if (!cmd) return false;
        if (cmd->state != ChildCommandState::Created) return false;

        cmd->state = ChildCommandState::Sent;
        cmd->sent_time_ns = timestamp_ns;
        cmd->last_update_ns = timestamp_ns;

        metrics_.sent.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool mark_acked(uint64_t command_id, uint64_t timestamp_ns) {
        auto* cmd = find_command(command_id);
        if (!cmd) return false;

        if (cmd->state != ChildCommandState::Sent &&
            cmd->state != ChildCommandState::Created) {
            return false;
        }

        cmd->state = ChildCommandState::Acked;
        cmd->last_update_ns = timestamp_ns;

        metrics_.acked.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool mark_failed(
        uint64_t command_id,
        std::string reason,
        uint64_t timestamp_ns
    ) {
        auto* cmd = find_command(command_id);
        if (!cmd) return false;
        if (cmd->state == ChildCommandState::Acked) return false;

        cmd->state = ChildCommandState::Failed;
        cmd->failure_reason = std::move(reason);
        cmd->last_update_ns = timestamp_ns;

        metrics_.failed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::vector<uint64_t> expire_stale_commands(
        uint64_t now_ns,
        uint64_t timeout_ns
    ) {
        std::vector<uint64_t> expired;

        for (auto& [id, cmd] : commands_) {
            if (cmd.state == ChildCommandState::Acked ||
                cmd.state == ChildCommandState::Failed ||
                cmd.state == ChildCommandState::Expired) {
                continue;
            }

            if (cmd.last_update_ns == 0 || now_ns < cmd.last_update_ns) continue;

            if (now_ns - cmd.last_update_ns >= timeout_ns) {
                cmd.state = ChildCommandState::Expired;
                cmd.last_update_ns = now_ns;
                expired.push_back(id);
                metrics_.expired.fetch_add(1, std::memory_order_relaxed);
            }
        }

        return expired;
    }

    std::optional<ChildOrderCommand> command(uint64_t command_id) const {
        auto it = commands_.find(command_id);
        if (it == commands_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<ChildOrderCommand> command_by_idempotency_key(
        const std::string& key
    ) const {
        auto it = idempotency_to_command_.find(key);
        if (it == idempotency_to_command_.end()) return std::nullopt;
        return command(it->second);
    }

    const CommandMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    ChildOrderCommand make_command(
        ChildCommandType type,
        const ChildOrder& child,
        double quantity,
        double price,
        uint64_t timestamp_ns
    ) {
        ChildOrderCommand cmd;

        cmd.command_id = next_command_id_.fetch_add(1, std::memory_order_relaxed);
        cmd.type = type;
        cmd.state = ChildCommandState::Created;

        cmd.parent_id = child.parent_id;
        cmd.child_id = child.child_id;
        cmd.venue = child.venue;
        cmd.symbol = child.symbol;
        cmd.side = child.side;
        cmd.quantity = quantity;
        cmd.price = price;

        cmd.client_order_id = child.client_order_id;
        cmd.venue_order_id = child.venue_order_id;

        cmd.create_time_ns = timestamp_ns;
        cmd.last_update_ns = timestamp_ns;

        cmd.idempotency_key = make_idempotency_key(cmd);

        return cmd;
    }

    bool register_command(const ChildOrderCommand& cmd) {
        if (cmd.idempotency_key.empty()) return false;

        if (idempotency_to_command_.contains(cmd.idempotency_key)) {
            metrics_.duplicates_blocked.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        commands_[cmd.command_id] = cmd;
        idempotency_to_command_[cmd.idempotency_key] = cmd.command_id;

        metrics_.created.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static std::string make_idempotency_key(const ChildOrderCommand& cmd) {
        return std::to_string(static_cast<int>(cmd.type)) + ":" +
               std::to_string(cmd.parent_id) + ":" +
               std::to_string(cmd.child_id) + ":" +
               cmd.venue + ":" +
               cmd.symbol + ":" +
               std::to_string(cmd.quantity) + ":" +
               std::to_string(cmd.price);
    }

    static std::string select_fallback_venue(
        const ChildOrder& failed,
        const std::vector<std::string>& fallback_venues
    ) {
        for (const auto& venue : fallback_venues) {
            if (!venue.empty() && venue != failed.venue) return venue;
        }

        return failed.venue;
    }

    ChildOrderCommand* find_command(uint64_t command_id) {
        auto it = commands_.find(command_id);
        if (it == commands_.end()) return nullptr;
        return &it->second;
    }

private:
    ChildOrderManager& manager_;
    RetryPolicy retry_policy_;

    std::atomic<uint64_t> next_command_id_{1};

    std::unordered_map<uint64_t, ChildOrderCommand> commands_;
    std::unordered_map<std::string, uint64_t> idempotency_to_command_;

    CommandMetrics metrics_{};
};

} // namespace hft::execution