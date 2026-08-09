#include "telemetry/net/tcp_client.hpp"

namespace telemetry::net {

void TcpClient::connect(const std::string& host, std::uint16_t port,
                        std::chrono::milliseconds connect_timeout,
                        std::chrono::milliseconds send_timeout) {
    FileDescriptor fd = connect_tcp(host, port, connect_timeout);
    set_send_timeout(fd.get(), send_timeout);
    fd_ = std::move(fd);
}

bool TcpClient::send_line(std::string_view line) {
    scratch_.assign(line);
    scratch_.push_back('\n');
    return send_raw(scratch_);
}

bool TcpClient::send_raw(std::string_view bytes) {
    if (!fd_.valid()) {
        return false;
    }
    if (!send_all(fd_.get(), bytes.data(), bytes.size())) {
        fd_.reset();
        return false;
    }
    return true;
}

}  // namespace telemetry::net
