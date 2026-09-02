#include <gtest/gtest.h>

#include <sstream>

#include "telemetry/common/logger.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

TEST(Logger, FormatsLogfmtLine) {
    const std::string line = Logger::format_line(
        1'700'000'000'123'456, LogLevel::Warn, "net", "client_connected",
        {{"conn", 3}, {"peer", "127.0.0.1:5000"}, {"ok", true}, {"ratio", 0.5}});
    EXPECT_EQ(line,
              "2023-11-14T22:13:20.123456Z level=warn component=net event=client_connected "
              "conn=3 peer=127.0.0.1:5000 ok=true ratio=0.5");
}

TEST(Logger, QuotesAndEscapesValues) {
    const std::string line = Logger::format_line(0, LogLevel::Info, "c", "e",
                                                 {{"msg", "hello \"world\"\n"}, {"empty", ""}});
    EXPECT_NE(line.find(R"(msg="hello \"world\"\n")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"(empty="")"), std::string::npos) << line;
}

TEST(Logger, RespectsLevel) {
    std::ostringstream out;
    Logger logger(LogLevel::Warn, &out);
    logger.info("c", "hidden");
    logger.warn("c", "shown");
    logger.error("c", "also_shown");
    const std::string text = out.str();
    EXPECT_EQ(text.find("hidden"), std::string::npos);
    EXPECT_NE(text.find("event=shown"), std::string::npos);
    EXPECT_NE(text.find("event=also_shown"), std::string::npos);

    logger.set_level(LogLevel::Off);
    EXPECT_FALSE(logger.enabled(LogLevel::Error));
}

TEST(Logger, ParseLevel) {
    EXPECT_EQ(parse_log_level("debug"), LogLevel::Debug);
    EXPECT_EQ(parse_log_level("off"), LogLevel::Off);
    EXPECT_FALSE(parse_log_level("verbose").has_value());
}

TEST(RateLimiter, AllowsBudgetPerWindowAndReportsSuppressed) {
    RateLimiter limiter(2, 1000ms);
    const SteadyTime t0 = SteadyTime{} + 10s;
    std::uint64_t suppressed = 99;
    EXPECT_TRUE(limiter.allow(suppressed, t0));
    EXPECT_EQ(suppressed, 0u);
    EXPECT_TRUE(limiter.allow(suppressed, t0 + 1ms));
    EXPECT_FALSE(limiter.allow(suppressed, t0 + 2ms));
    EXPECT_FALSE(limiter.allow(suppressed, t0 + 3ms));
    EXPECT_FALSE(limiter.allow(suppressed, t0 + 4ms));
    // New window: allowed again, and told how many were dropped.
    EXPECT_TRUE(limiter.allow(suppressed, t0 + 1001ms));
    EXPECT_EQ(suppressed, 3u);
}
