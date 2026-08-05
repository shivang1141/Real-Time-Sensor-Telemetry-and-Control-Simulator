#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace telemetry::protocol {

/// Splits a TCP byte stream into '\n'-terminated frames.
///
/// TCP delivers bytes, not messages: one recv() may contain half a frame or several
/// frames. The framer buffers at most `max_frame_bytes` of an incomplete frame, so
/// memory per connection is bounded regardless of what the peer sends.
///
/// A frame longer than the limit is reported via on_oversized() and its bytes are
/// discarded up to and including the next '\n', after which framing resynchronizes.
/// A trailing '\r' is stripped. Empty lines are ignored.
class LineFramer {
public:
    explicit LineFramer(std::size_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {
        buffer_.reserve(max_frame_bytes);
    }

    template <typename OnFrame, typename OnOversized>
    void feed(const char* data, std::size_t size, OnFrame&& on_frame, OnOversized&& on_oversized) {
        std::size_t pos = 0;
        while (pos < size) {
            const void* found = std::memchr(data + pos, '\n', size - pos);
            const std::size_t end =
                found != nullptr ? static_cast<std::size_t>(static_cast<const char*>(found) - data)
                                 : size;
            const std::size_t chunk = end - pos;

            if (discarding_) {
                if (found != nullptr) {
                    discarding_ = false;
                }
                pos = found != nullptr ? end + 1 : size;
                continue;
            }

            if (buffer_.size() + chunk > max_frame_bytes_) {
                buffer_.clear();
                on_oversized();
                discarding_ = found == nullptr;
                pos = found != nullptr ? end + 1 : size;
                continue;
            }

            if (found == nullptr) {
                buffer_.append(data + pos, chunk);
                return;
            }

            // Zero-copy fast path when the frame is entirely inside this chunk.
            std::string_view frame;
            if (buffer_.empty()) {
                frame = std::string_view(data + pos, chunk);
            } else {
                buffer_.append(data + pos, chunk);
                frame = buffer_;
            }
            if (!frame.empty() && frame.back() == '\r') {
                frame.remove_suffix(1);
            }
            if (!frame.empty()) {
                on_frame(frame);
            }
            buffer_.clear();
            pos = end + 1;
        }
    }

    /// Bytes of an incomplete frame currently buffered.
    std::size_t buffered_bytes() const { return buffer_.size(); }
    bool discarding() const { return discarding_; }

private:
    std::size_t max_frame_bytes_;
    std::string buffer_;
    bool discarding_{false};
};

}  // namespace telemetry::protocol
