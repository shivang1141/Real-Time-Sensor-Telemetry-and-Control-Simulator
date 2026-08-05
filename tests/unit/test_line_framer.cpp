#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "telemetry/protocol/line_framer.hpp"

using telemetry::protocol::LineFramer;

namespace {

struct Collector {
    std::vector<std::string> frames;
    int oversized = 0;

    void feed(LineFramer& framer, const std::string& bytes) {
        framer.feed(
            bytes.data(), bytes.size(), [this](std::string_view f) { frames.emplace_back(f); },
            [this] { ++oversized; });
    }
};

}  // namespace

TEST(LineFramer, SplitsMultipleFramesInOneChunk) {
    LineFramer framer(64);
    Collector c;
    c.feed(framer, "a\nbb\nccc\n");
    EXPECT_EQ(c.frames, (std::vector<std::string>{"a", "bb", "ccc"}));
    EXPECT_EQ(framer.buffered_bytes(), 0u);
}

TEST(LineFramer, ReassemblesFrameSplitAcrossChunks) {
    LineFramer framer(64);
    Collector c;
    c.feed(framer, "hel");
    EXPECT_TRUE(c.frames.empty());
    EXPECT_EQ(framer.buffered_bytes(), 3u);
    c.feed(framer, "lo wor");
    c.feed(framer, "ld\nnext");
    EXPECT_EQ(c.frames, (std::vector<std::string>{"hello world"}));
    EXPECT_EQ(framer.buffered_bytes(), 4u);
}

TEST(LineFramer, ByteAtATimeDelivery) {
    LineFramer framer(64);
    Collector c;
    const std::string stream = "first\nsecond\n";
    for (char ch : stream) {
        c.feed(framer, std::string(1, ch));
    }
    EXPECT_EQ(c.frames, (std::vector<std::string>{"first", "second"}));
}

TEST(LineFramer, StripsCarriageReturnAndSkipsEmptyLines) {
    LineFramer framer(64);
    Collector c;
    c.feed(framer, "a\r\n\n\r\nb\n");
    EXPECT_EQ(c.frames, (std::vector<std::string>{"a", "b"}));
}

TEST(LineFramer, FrameExactlyAtLimitIsAccepted) {
    LineFramer framer(8);
    Collector c;
    c.feed(framer, "12345678\n");
    EXPECT_EQ(c.frames, (std::vector<std::string>{"12345678"}));
    EXPECT_EQ(c.oversized, 0);
}

TEST(LineFramer, OversizedFrameInOneChunkIsDiscardedAndFramingResumes) {
    LineFramer framer(8);
    Collector c;
    c.feed(framer, "123456789\nok\n");
    EXPECT_EQ(c.oversized, 1);
    EXPECT_EQ(c.frames, (std::vector<std::string>{"ok"}));
}

TEST(LineFramer, OversizedFrameAcrossChunksBoundsMemory) {
    LineFramer framer(8);
    Collector c;
    c.feed(framer, "12345");
    c.feed(framer, "67890");  // Exceeds the limit without a newline.
    EXPECT_EQ(c.oversized, 1);
    EXPECT_TRUE(framer.discarding());
    EXPECT_EQ(framer.buffered_bytes(), 0u);
    // A long tail without newline keeps being discarded without buffering.
    c.feed(framer, std::string(10'000, 'x'));
    EXPECT_EQ(framer.buffered_bytes(), 0u);
    EXPECT_EQ(c.oversized, 1) << "one oversized frame is reported once";
    c.feed(framer, "tail\ngood\n");
    EXPECT_FALSE(framer.discarding());
    EXPECT_EQ(c.frames, (std::vector<std::string>{"good"}));
}
