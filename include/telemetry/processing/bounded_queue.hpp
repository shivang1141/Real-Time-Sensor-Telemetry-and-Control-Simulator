#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace telemetry {

/// Bounded multi-producer / multi-consumer FIFO queue (mutex + two condition variables).
///
/// Design notes:
///  * Capacity is fixed, so memory is bounded under overload. Producers choose the
///    overload policy: try_push() fails fast (the server uses it and counts a drop),
///    push() blocks for backpressure.
///  * close() is the shutdown signal: pushes fail afterwards, blocked producers and
///    consumers wake up, and consumers keep draining remaining items until the queue is
///    empty, after which pop() returns std::nullopt. No item accepted before close() is
///    lost.
///  * Notifications happen after releasing the lock to avoid waking a thread only for it
///    to block on the mutex again.
///  * Two optimizations were added after benchmarking showed consumer starvation under
///    overload (see docs/PERFORMANCE.md):
///      - try_push() rejects without taking the mutex when an atomic copy of the size
///        says the queue is full, so producers hammering a full queue do not compete
///        with the consumer for the lock. The copy is updated under the lock and may be
///        momentarily stale; the only effect is that an item can be dropped just as a
///        slot frees up. Capacity is still enforced exactly under the lock.
///      - pop_batch() lets a consumer drain many items per lock acquisition.
///  * The queue stays mutex-based rather than lock-free: with the two changes above it is
///    no longer the bottleneck, and it is far easier to reason about.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be > 0");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    /// Non-blocking. Returns false (leaving `item` untouched) if full or closed.
    bool try_push(T&& item) {
        if (size_hint_.load(std::memory_order_relaxed) >= capacity_) {
            return false;  // Fast rejection without touching the mutex.
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || items_.size() >= capacity_) {
                return false;
            }
            enqueue_locked(std::move(item));
        }
        not_empty_.notify_one();
        return true;
    }

    /// Blocks while full. Returns false if the queue is (or becomes) closed.
    bool push(T&& item) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
            if (closed_) {
                return false;
            }
            enqueue_locked(std::move(item));
        }
        not_empty_.notify_one();
        return true;
    }

    /// Blocks until an item is available or the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        return dequeue_locked(lock);
    }

    /// Like pop() but gives up after `timeout`. std::nullopt means timeout, or closed and
    /// empty; check closed() to tell them apart.
    template <typename Rep, typename Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
        return dequeue_locked(lock);
    }

    std::optional<T> try_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        return dequeue_locked(lock);
    }

    /// Waits up to `timeout` for at least one item, then moves up to `max_items` into
    /// `out` (appending) under a single lock acquisition. Returns the number moved; 0
    /// means timeout, or closed and empty.
    template <typename Rep, typename Period>
    std::size_t pop_batch(std::vector<T>& out, std::size_t max_items,
                          std::chrono::duration<Rep, Period> timeout) {
        std::size_t moved = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
            while (moved < max_items && !items_.empty()) {
                out.push_back(std::move(items_.front()));
                items_.pop_front();
                ++moved;
            }
            size_hint_.store(items_.size(), std::memory_order_relaxed);
        }
        if (moved > 0) {
            not_full_.notify_all();
        }
        return moved;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    /// Largest size observed since construction.
    std::size_t high_water_mark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_mark_;
    }

    std::size_t capacity() const { return capacity_; }

private:
    void enqueue_locked(T&& item) {
        items_.push_back(std::move(item));
        size_hint_.store(items_.size(), std::memory_order_relaxed);
        high_water_mark_ = std::max(high_water_mark_, items_.size());
    }

    std::optional<T> dequeue_locked(std::unique_lock<std::mutex>& lock) {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> item(std::move(items_.front()));
        items_.pop_front();
        size_hint_.store(items_.size(), std::memory_order_relaxed);
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    std::atomic<std::size_t> size_hint_{0};  ///< Copy of items_.size() for lock-free checks.
    std::size_t high_water_mark_{0};
    bool closed_{false};
};

}  // namespace telemetry
