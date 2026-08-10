#include "telemetry/net/tcp_server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <vector>

#include "telemetry/protocol/line_framer.hpp"

namespace telemetry::net {

namespace {
constexpr int kAcceptPollTimeoutMs = 500;  // Upper bound on how long finished threads linger.
constexpr const char* kComponent = "net";
}  // namespace

std::string_view to_string(DisconnectReason reason) {
    switch (reason) {
        case DisconnectReason::PeerClosed:
            return "peer_closed";
        case DisconnectReason::IdleTimeout:
            return "idle_timeout";
        case DisconnectReason::Error:
            return "error";
        case DisconnectReason::ServerShutdown:
            return "server_shutdown";
    }
    return "unknown";
}

TcpServer::TcpServer(TcpServerConfig config, ConnectionHandler& handler, Logger& logger)
    : config_(std::move(config)), handler_(handler), logger_(logger) {}

TcpServer::~TcpServer() { stop(); }

void TcpServer::start() {
    if (running_.load()) {
        return;
    }
    listen_fd_ = listen_tcp(config_.bind_address, config_.port, config_.backlog);
    bound_port_ = local_port(listen_fd_.get());
    running_.store(true);
    acceptor_ = std::thread([this] { accept_loop(); });
    logger_.info(kComponent, "server_listening",
                 {{"address", config_.bind_address},
                  {"port", bound_port_},
                  {"max_connections", config_.max_connections}});
}

void TcpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stop_event_.signal();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    // The acceptor has exited, so no new connections can be added.
    std::list<std::unique_ptr<Connection>> remaining;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        remaining.swap(connections_);
    }
    for (auto& connection : remaining) {
        if (connection->thread.joinable()) {
            connection->thread.join();
        }
    }
    listen_fd_.reset();
    logger_.info(kComponent, "server_stopped", {{"closed_connections", remaining.size()}});
}

void TcpServer::accept_loop() {
    pollfd fds[2] = {{listen_fd_.get(), POLLIN, 0}, {stop_event_.fd(), POLLIN, 0}};
    while (true) {
        const int ready = ::poll(fds, 2, kAcceptPollTimeoutMs);
        reap_finished_connections();
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_.error(kComponent, "poll_failed", {{"error", std::strerror(errno)}});
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            break;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            accept_one();
        }
    }
}

void TcpServer::accept_one() {
    const int raw_fd = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
    if (raw_fd < 0) {
        const int error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ECONNABORTED) {
            return;
        }
        logger_.error(kComponent, "accept_failed", {{"error", std::strerror(error)}});
        if (error == EMFILE || error == ENFILE) {
            // Out of descriptors: back off instead of spinning on a permanently readable
            // listening socket.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return;
    }
    FileDescriptor fd(raw_fd);
    std::string peer = peer_name(fd.get());

    if (active_.load() >= config_.max_connections) {
        logger_.warn(kComponent, "connection_rejected",
                     {{"peer", peer}, {"reason", "max_connections"},
                      {"limit", config_.max_connections}});
        handler_.on_connection_rejected(peer);
        return;  // fd closes here.
    }

    auto connection = std::make_unique<Connection>();
    connection->fd = std::move(fd);
    connection->peer = std::move(peer);

    std::string thread_error;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection->id = next_id_++;
        Connection& ref = *connection;
        active_.fetch_add(1);
        try {
            ref.thread = std::thread([this, &ref] { connection_loop(ref); });
            connections_.push_back(std::move(connection));
        } catch (const std::system_error& e) {
            active_.fetch_sub(1);
            thread_error = e.what();
        }
    }
    // Logged outside the lock: no code path holds two mutexes at once.
    if (!thread_error.empty()) {
        logger_.error(kComponent, "thread_create_failed", {{"error", thread_error}});
    }
}

void TcpServer::connection_loop(Connection& connection) {
    const ConnectionId id = connection.id;
    const int fd = connection.fd.get();
    logger_.info(kComponent, "client_connected", {{"conn", id}, {"peer", connection.peer}});
    handler_.on_connect(id, connection.peer);

    protocol::LineFramer framer(config_.max_frame_bytes);
    std::vector<char> buffer(config_.recv_buffer_bytes);
    const int timeout_ms =
        config_.idle_timeout.count() > 0 ? static_cast<int>(config_.idle_timeout.count()) : -1;

    pollfd fds[2] = {{fd, POLLIN, 0}, {stop_event_.fd(), POLLIN, 0}};
    DisconnectReason reason = DisconnectReason::PeerClosed;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;

    while (true) {
        // poll() restarts its timeout on every iteration, and every iteration follows a
        // read, so returning 0 means "no data for idle_timeout".
        const int ready = ::poll(fds, 2, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            reason = DisconnectReason::Error;
            break;
        }
        if (ready == 0) {
            reason = DisconnectReason::IdleTimeout;
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            reason = DisconnectReason::ServerShutdown;
            break;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
        }
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received == 0) {
            reason = DisconnectReason::PeerClosed;
            break;
        }
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            reason = DisconnectReason::Error;
            break;
        }
        bytes += static_cast<std::uint64_t>(received);
        const SteadyTime now = SteadyClock::now();
        framer.feed(
            buffer.data(), static_cast<std::size_t>(received),
            [&](std::string_view frame) {
                ++frames;
                handler_.on_frame(id, frame, now);
            },
            [&] { handler_.on_oversized_frame(id); });
    }

    if (framer.buffered_bytes() > 0) {
        logger_.debug(kComponent, "partial_frame_discarded",
                      {{"conn", id}, {"bytes", framer.buffered_bytes()}});
    }
    ::shutdown(fd, SHUT_RDWR);
    handler_.on_disconnect(id, reason);
    logger_.info(kComponent, "client_disconnected",
                 {{"conn", id}, {"peer", connection.peer}, {"reason", to_string(reason)},
                  {"frames", frames}, {"bytes", bytes}});
    active_.fetch_sub(1);
    connection.finished.store(true, std::memory_order_release);
}

void TcpServer::reap_finished_connections() {
    std::list<std::unique_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Join outside the lock; these threads have already finished their work.
    for (auto& connection : finished) {
        connection->thread.join();
    }
}

}  // namespace telemetry::net
