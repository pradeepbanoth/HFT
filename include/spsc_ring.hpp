#pragma once

#include <array>
#include <atomic>
#include <thread>
#include <cstddef>
#include <utility>

namespace hft {

template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    void push(T&& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) & mask_;

        while (next == tail_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
    }

    bool pop(T& out) {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

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
    std::array<T, Capacity> buffer_{};
};

} // namespace hft