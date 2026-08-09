#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "telemetry/common/clock.hpp"
#include "telemetry/common/logger.hpp"
#include "telemetry/net/socket.hpp"

namespace telemetry::net {

using ConnectionId = std::uint64_t;

enum class DisconnectReason { PeerClosed, IdleTimeout, Error, ServerShutdown };
std::string_view to_string(DisconnectReason reason);

struct TcpServerConfig {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{9000};  ///< 0 selects an ephemeral port (see TcpServer::port()).
    int backlog{128};
    std::size_t max_connections{256};
    std::size_t max_frame_bytes{1024};
    std::size_t recv_buffer_bytes{16 * 1024};
    std::chrono::milliseconds idle_timeout{30'000};  ///< 0 disables the idle timeout.
};

/// Receives connection events. Every callback for a given connection runs on that
/// connection's thread; callbacks for different connections run concurrently, so
/// implementations must be thread-safe.
class ConnectionHandler {
public:
    virtual ~ConnectionHandler() = default;
    virtual void on_connect(ConnectionId /*id*/, const std::string& /*peer*/) {}
    /// `received_at` is the steady time at which the frame's final byte was read.
    virtual void on_frame(ConnectionId id, std::string_view frame, SteadyTime received_at) = 0;
    virtual void on_oversized_frame(ConnectionId /*id*/) {}
    virtual void on_disconnect(ConnectionId /*id*/, DisconnectReason /*reason*/) {}
    virtual void on_connection_rejected(const std::string& /*peer*/) {}
};

/// Multi-client TCP server for newline-delimited frames.
///
/// Thread model: one acceptor thread plus one thread per connection. Each connection
/// thread blocks in poll() on its socket and on a shared shutdown eventfd, reads into a
/// fixed buffer, splits frames with a bounded LineFramer and hands each frame to the
/// handler. Thread-per-connection keeps the code simple and is adequate for the
/// hundreds of connections this simulator targets; max_connections bounds the thread
/// count. Connections beyond the limit are accepted and immediately closed so clients
/// see a definite failure instead of a stalled handshake.
class TcpServer {
public:
    TcpServer(TcpServerConfig config, ConnectionHandler& handler, Logger& logger);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /// Binds and starts accepting. Throws std::system_error if the bind fails.
    void start();

    /// Stops accepting, closes every connection and joins all threads. Idempotent.
    void stop();

    std::uint16_t port() const { return bound_port_; }
    std::size_t active_connections() const { return active_.load(std::memory_order_relaxed); }

private:
    struct Connection {
        ConnectionId id{0};
        FileDescriptor fd;
        std::string peer;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void accept_loop();
    void accept_one();
    void connection_loop(Connection& connection);
    void reap_finished_connections();

    TcpServerConfig config_;
    ConnectionHandler& handler_;
    Logger& logger_;

    FileDescriptor listen_fd_;
    WakeupEvent stop_event_;
    std::uint16_t bound_port_{0};
    std::atomic<bool> running_{false};
    std::thread acceptor_;

    std::mutex connections_mutex_;
    std::list<std::unique_ptr<Connection>> connections_;
    std::atomic<std::size_t> active_{0};
    ConnectionId next_id_{1};
};

}  // namespace telemetry::net
