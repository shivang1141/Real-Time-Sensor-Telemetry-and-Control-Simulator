#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include "telemetry/processing/bounded_queue.hpp"

using telemetry::BoundedQueue;
using namespace std::chrono_literals;

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, FifoOrder) {
    BoundedQueue<int> q(4);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_push(int{i}));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(q.try_pop(), i);
    }
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(BoundedQueue, TryPushFailsWhenFullAndLeavesItemIntact) {
    BoundedQueue<std::string> q(1);
    ASSERT_TRUE(q.try_push("a"));
    std::string item = "b";
    EXPECT_FALSE(q.try_push(std::move(item)));
    EXPECT_EQ(item, "b") << "a rejected item must not be moved from";
    EXPECT_EQ(q.size(), 1u);
}

TEST(BoundedQueue, HighWaterMarkTracksPeak) {
    BoundedQueue<int> q(10);
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);
    q.try_pop();
    q.try_pop();
    q.try_push(4);
    EXPECT_EQ(q.high_water_mark(), 3u);
}

TEST(BoundedQueue, PopForTimesOut) {
    BoundedQueue<int> q(1);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(q.pop_for(20ms).has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 15ms);
    EXPECT_FALSE(q.closed());
}

TEST(BoundedQueue, CloseDrainsRemainingItemsThenReturnsNullopt) {
    BoundedQueue<int> q(4);
    q.try_push(1);
    q.try_push(2);
    q.close();
    EXPECT_FALSE(q.try_push(3)) << "push after close must fail";
    EXPECT_EQ(q.pop(), 1);
    EXPECT_EQ(q.pop(), 2);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(BoundedQueue, CloseWakesBlockedConsumer) {
    BoundedQueue<int> q(1);
    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        EXPECT_FALSE(q.pop().has_value());
        returned = true;
    });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(returned.load());
    q.close();
    consumer.join();
    EXPECT_TRUE(returned.load());
}

TEST(BoundedQueue, CloseWakesBlockedProducer) {
    BoundedQueue<int> q(1);
    q.try_push(1);
    std::thread producer([&] { EXPECT_FALSE(q.push(2)); });
    std::this_thread::sleep_for(20ms);
    q.close();
    producer.join();
}

TEST(BoundedQueue, BlockingPushWaitsForSpace) {
    BoundedQueue<int> q(1);
    q.try_push(1);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        EXPECT_TRUE(q.push(2));
        pushed = true;
    });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(pushed.load()) << "push must block while full";
    EXPECT_EQ(q.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(q.pop(), 2);
}

TEST(BoundedQueue, PopBatchDrainsUpToMaxInOrder) {
    BoundedQueue<int> q(10);
    for (int i = 0; i < 7; ++i) q.try_push(int{i});
    std::vector<int> out;
    EXPECT_EQ(q.pop_batch(out, 5, 10ms), 5u);
    EXPECT_EQ(out, (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_EQ(q.pop_batch(out, 5, 10ms), 2u);
    EXPECT_EQ(out, (std::vector<int>{0, 1, 2, 3, 4, 5, 6})) << "pop_batch appends";
    EXPECT_EQ(q.size(), 0u);
}

TEST(BoundedQueue, PopBatchTimesOutAndReportsClosedEmpty) {
    BoundedQueue<int> q(4);
    std::vector<int> out;
    EXPECT_EQ(q.pop_batch(out, 8, 10ms), 0u);
    EXPECT_FALSE(q.closed());
    q.try_push(1);
    q.close();
    EXPECT_EQ(q.pop_batch(out, 8, 10ms), 1u) << "items queued before close are drained";
    EXPECT_EQ(q.pop_batch(out, 8, 10ms), 0u);
    EXPECT_TRUE(q.closed());
}

TEST(BoundedQueue, PopBatchFreesCapacityForTryPush) {
    BoundedQueue<int> q(2);
    q.try_push(1);
    q.try_push(2);
    EXPECT_FALSE(q.try_push(3)) << "fast-path rejection when full";
    std::vector<int> out;
    q.pop_batch(out, 2, 10ms);
    EXPECT_TRUE(q.try_push(3)) << "size hint must be refreshed by pop_batch";
}

TEST(BoundedQueue, ConcurrentTryPushNeverExceedsCapacity) {
    BoundedQueue<int> q(16);
    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            while (!stop.load()) {
                if (q.try_push(1)) accepted.fetch_add(1);
            }
        });
    }
    int consumed = 0;
    std::vector<int> out;
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < deadline) {
        out.clear();
        consumed += static_cast<int>(q.pop_batch(out, 8, 1ms));
    }
    stop = true;
    for (auto& t : producers) t.join();
    out.clear();
    while (q.pop_batch(out, 64, 0ms) > 0) {
    }
    consumed += static_cast<int>(out.size());
    EXPECT_EQ(consumed, accepted.load());
    EXPECT_LE(q.high_water_mark(), 16u);
}

// Stress test: run under ThreadSanitizer to detect data races. Verifies that every item
// pushed by any producer is consumed exactly once.
TEST(BoundedQueue, ConcurrentProducersAndConsumersDeliverEveryItemOnce) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 20'000;
    BoundedQueue<int> q(64);
    std::vector<std::atomic<int>> seen(kProducers * kPerProducer);

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (auto item = q.pop()) {
                seen[static_cast<std::size_t>(*item)].fetch_add(1);
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                ASSERT_TRUE(q.push(p * kPerProducer + i));
            }
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    for (std::size_t i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "item " << i;
    }
    EXPECT_LE(q.high_water_mark(), q.capacity());
}
