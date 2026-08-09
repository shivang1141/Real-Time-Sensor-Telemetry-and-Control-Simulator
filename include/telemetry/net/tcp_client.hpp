#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "telemetry/net/socket.hpp"

namespace telemetry::net {

/// Blocking TCP client used by the sensor simulator, load generator and tests.
/// Not thread-safe: each sending thread owns its own client.
class TcpClient {
public:
    /// Throws std::system_error if the connection cannot be established.
    void connect(const std::string& host, std::uint16_t port,
                 std::chrono::milliseconds connect_timeout = std::chrono::seconds(1),
                 std::chrono::milliseconds send_timeout = std::chrono::seconds(2));

    /// Sends `line` followed by '\n' in a single write. Returns false and closes the
    /// connection if the write fails.
    bool send_line(std::string_view line);

    /// Sends bytes exactly as given (no framing). Returns false and closes on failure.
    bool send_raw(std::string_view bytes);

    void close() noexcept { fd_.reset(); }
    bool connected() const noexcept { return fd_.valid(); }
    int fd() const noexcept { return fd_.get(); }

private:
    FileDescriptor fd_;
    std::string scratch_;
};

}  // namespace telemetry::net
