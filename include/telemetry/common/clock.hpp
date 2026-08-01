#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace telemetry {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;

/// Time source abstraction so that timing-dependent logic (staleness, timestamp
/// validation) can be tested deterministically.
///
/// steady_now() is monotonic and is the only clock used for measuring durations.
/// wall_now_us() is Unix-epoch microseconds and is used only for message timestamps,
/// which must be comparable across processes.
class Clock {
public:
    virtual ~Clock() = default;
    virtual SteadyTime steady_now() const = 0;
    virtual std::int64_t wall_now_us() const = 0;
};

class SystemClock final : public Clock {
public:
    SteadyTime steady_now() const override { return SteadyClock::now(); }

    std::int64_t wall_now_us() const override {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    }
};

/// Manually advanced clock for tests. Both time bases advance together. Thread-safe.
class ManualClock final : public Clock {
public:
    explicit ManualClock(std::int64_t wall_start_us = 1'700'000'000'000'000)
        : wall_start_us_(wall_start_us) {}

    SteadyTime steady_now() const override {
        return SteadyTime{} + std::chrono::microseconds(offset_us_.load());
    }

    std::int64_t wall_now_us() const override { return wall_start_us_ + offset_us_.load(); }

    void advance(std::chrono::microseconds delta) { offset_us_.fetch_add(delta.count()); }

private:
    std::int64_t wall_start_us_;
    std::atomic<std::int64_t> offset_us_{0};
};

}  // namespace telemetry
