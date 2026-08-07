#include "telemetry/net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>

namespace telemetry::net {

namespace {

[[noreturn]] void throw_errno(int error, const std::string& what) {
    throw std::system_error(error, std::generic_category(), what);
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

AddrInfoPtr resolve(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc =
        getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::system_error(std::make_error_code(std::errc::host_unreachable),
                                "resolve " + host + ": " + gai_strerror(rc));
    }
    return AddrInfoPtr(result);
}

void set_option(int fd, int level, int name, int value) {
    if (setsockopt(fd, level, name, &value, sizeof value) != 0) {
        throw_errno(errno, "setsockopt");
    }
}

void set_nonblocking(int fd, bool enabled) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw_errno(errno, "fcntl(F_GETFL)");
    }
    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, updated) != 0) {
        throw_errno(errno, "fcntl(F_SETFL)");
    }
}

std::string endpoint(const std::string& host, std::uint16_t port) {
    return host + ":" + std::to_string(port);
}

}  // namespace

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

WakeupEvent::WakeupEvent() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!fd_.valid()) {
        throw_errno(errno, "eventfd");
    }
}

void WakeupEvent::signal() noexcept {
    const std::uint64_t one = 1;
    // A failed write can only mean the counter is saturated, i.e. already signalled.
    [[maybe_unused]] const auto written = ::write(fd_.get(), &one, sizeof one);
}

FileDescriptor listen_tcp(const std::string& bind_address, std::uint16_t port, int backlog) {
    const AddrInfoPtr addresses = resolve(bind_address, port, /*passive=*/true);
    int last_error = 0;
    for (const addrinfo* ai = addresses.get(); ai != nullptr; ai = ai->ai_next) {
        FileDescriptor fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd.valid()) {
            last_error = errno;
            continue;
        }
        set_option(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1);
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0 ||
            ::listen(fd.get(), backlog) != 0) {
            last_error = errno;
            continue;
        }
        set_nonblocking(fd.get(), true);
        return fd;
    }
    throw_errno(last_error, "listen " + endpoint(bind_address, port));
}

FileDescriptor connect_tcp(const std::string& host, std::uint16_t port,
                           std::chrono::milliseconds timeout) {
    const AddrInfoPtr addresses = resolve(host, port, /*passive=*/false);
    int last_error = ECONNREFUSED;
    for (const addrinfo* ai = addresses.get(); ai != nullptr; ai = ai->ai_next) {
        FileDescriptor fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                   ai->ai_protocol));
        if (!fd.valid()) {
            last_error = errno;
            continue;
        }
        if (::connect(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                last_error = errno;
                continue;
            }
            pollfd pfd{fd.get(), POLLOUT, 0};
            const int ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
            if (ready <= 0) {
                last_error = ready == 0 ? ETIMEDOUT : errno;
                continue;
            }
            int so_error = 0;
            socklen_t len = sizeof so_error;
            if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
                last_error = so_error != 0 ? so_error : errno;
                continue;
            }
        }
        set_nonblocking(fd.get(), false);
        set_option(fd.get(), IPPROTO_TCP, TCP_NODELAY, 1);
        return fd;
    }
    throw_errno(last_error, "connect " + endpoint(host, port));
}

std::uint16_t local_port(int fd) {
    sockaddr_storage address{};
    socklen_t len = sizeof address;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
        throw_errno(errno, "getsockname");
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}

std::string peer_name(int fd) {
    sockaddr_storage address{};
    socklen_t len = sizeof address;
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
        return "unknown";
    }
    char host[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (address.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
        port = ntohs(in6->sin6_port);
    } else {
        const auto* in4 = reinterpret_cast<const sockaddr_in*>(&address);
        inet_ntop(AF_INET, &in4->sin_addr, host, sizeof host);
        port = ntohs(in4->sin_port);
    }
    return endpoint(host, port);
}

void set_send_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
        throw_errno(errno, "setsockopt(SO_SNDTIMEO)");
    }
}

bool send_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace telemetry::net
