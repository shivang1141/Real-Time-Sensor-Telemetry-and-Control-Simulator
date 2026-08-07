#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace telemetry::net {

/// Move-only owner of a POSIX file descriptor; closes it on destruction.
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_{-1};
};

/// eventfd used to interrupt blocking poll() calls during shutdown. It is
/// level-triggered and never drained, so a single signal() wakes every thread that
/// polls it, now or later.
class WakeupEvent {
public:
    WakeupEvent();
    void signal() noexcept;
    int fd() const noexcept { return fd_.get(); }

private:
    FileDescriptor fd_;
};

/// Creates a non-blocking listening TCP socket. Port 0 picks an ephemeral port.
/// Throws std::system_error on failure.
FileDescriptor listen_tcp(const std::string& bind_address, std::uint16_t port, int backlog);

/// Connects with a timeout and returns a blocking socket with TCP_NODELAY set.
/// Throws std::system_error on failure.
FileDescriptor connect_tcp(const std::string& host, std::uint16_t port,
                           std::chrono::milliseconds timeout);

std::uint16_t local_port(int fd);
std::string peer_name(int fd);
void set_send_timeout(int fd, std::chrono::milliseconds timeout);

/// Writes the whole buffer, looping over partial writes and EINTR. Uses MSG_NOSIGNAL so
/// a closed peer yields an error instead of SIGPIPE. Returns false on error or send
/// timeout; errno describes the failure.
bool send_all(int fd, const char* data, std::size_t size);

}  // namespace telemetry::net
