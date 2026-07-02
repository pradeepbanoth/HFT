#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace hft::exchange {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

struct alignas(CACHE_LINE_SIZE) RingBufferMetrics {
    std::atomic<uint64_t> pushes{0};
    std::atomic<uint64_t> pops{0};
    std::atomic<uint64_t> failed_pushes{0};
    std::atomic<uint64_t> failed_pops{0};
    std::atomic<uint64_t> batch_pushes{0};
    std::atomic<uint64_t> batch_pops{0};
};

template <typename T, std::size_t Capacity>
class LockFreeRingBuffer {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");

private:
    struct alignas(CACHE_LINE_SIZE) Cell {
        std::atomic<std::size_t> sequence;
        alignas(T) unsigned char storage[sizeof(T)];

        T* ptr() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }

        const T* ptr() const noexcept {
            return std::launder(reinterpret_cast<const T*>(storage));
        }
    };

public:
    LockFreeRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }

        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~LockFreeRingBuffer() {
        T value;
        while (try_pop(value)) {}
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&) = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&) = delete;

    template <typename... Args>
    bool emplace(Args&&... args) {
        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & mask_];

            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                metrics_.failed_pushes.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        ::new (static_cast<void*>(cell->storage)) T(std::forward<Args>(args)...);
        cell->sequence.store(pos + 1, std::memory_order_release);

        metrics_.pushes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool try_push(const T& value) {
        return emplace(value);
    }

    bool try_push(T&& value) {
        return emplace(std::move(value));
    }

    bool try_pop(T& out) {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & mask_];

            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                metrics_.failed_pops.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* item = cell->ptr();
        out = std::move(*item);
        item->~T();

        cell->sequence.store(pos + Capacity, std::memory_order_release);

        metrics_.pops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::optional<T> try_pop() {
        static_assert(std::is_move_constructible_v<T>,
                      "T must be move constructible for optional pop");

        T out;
        if (!try_pop(out)) {
            return std::nullopt;
        }

        return out;
    }

    template <typename InputIt>
    std::size_t push_batch(InputIt first, InputIt last) {
        std::size_t count = 0;

        for (; first != last; ++first) {
            if (!try_push(*first)) break;
            ++count;
        }

        if (count > 0) {
            metrics_.batch_pushes.fetch_add(1, std::memory_order_relaxed);
        }

        return count;
    }

    std::size_t push_batch(const std::vector<T>& values) {
        return push_batch(values.begin(), values.end());
    }

    std::size_t push_batch_move(std::vector<T>& values) {
        std::size_t count = 0;

        for (auto& value : values) {
            if (!try_push(std::move(value))) break;
            ++count;
        }

        if (count > 0) {
            metrics_.batch_pushes.fetch_add(1, std::memory_order_relaxed);
        }

        return count;
    }

    std::size_t pop_batch(std::vector<T>& out, std::size_t max_items) {
        std::size_t count = 0;
        out.reserve(out.size() + max_items);

        while (count < max_items) {
            auto item = try_pop();
            if (!item) break;

            out.push_back(std::move(*item));
            ++count;
        }

        if (count > 0) {
            metrics_.batch_pops.fetch_add(1, std::memory_order_relaxed);
        }

        return count;
    }

    bool empty() const noexcept {
        return size_approx() == 0;
    }

    bool full() const noexcept {
        return size_approx() >= Capacity;
    }

    std::size_t capacity() const noexcept {
        return Capacity;
    }

    std::size_t size_approx() const noexcept {
        const auto enq = enqueue_pos_.load(std::memory_order_acquire);
        const auto deq = dequeue_pos_.load(std::memory_order_acquire);

        if (enq >= deq) {
            const auto diff = enq - deq;
            return diff > Capacity ? Capacity : diff;
        }

        return 0;
    }

    const RingBufferMetrics& metrics() const noexcept {
        return metrics_;
    }

    uint64_t pushes() const noexcept {
        return metrics_.pushes.load(std::memory_order_relaxed);
    }

    uint64_t pops() const noexcept {
        return metrics_.pops.load(std::memory_order_relaxed);
    }

    uint64_t failed_pushes() const noexcept {
        return metrics_.failed_pushes.load(std::memory_order_relaxed);
    }

    uint64_t failed_pops() const noexcept {
        return metrics_.failed_pops.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    alignas(CACHE_LINE_SIZE) Cell cells_[Capacity];

    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> enqueue_pos_{0};

    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> dequeue_pos_{0};

    alignas(CACHE_LINE_SIZE) RingBufferMetrics metrics_{};
};

} // namespace hft::exchange