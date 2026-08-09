#include <gtest/gtest.h>
#include <sys/socket.h>

#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "support/test_helpers.hpp"
#include "telemetry/net/tcp_client.hpp"
#include "telemetry/net/tcp_server.hpp"

using namespace telemetry;
using namespace telemetry::net;
using namespace std::chrono_literals;

namespace {

/// Thread-safe handler that records everything the server reports.
class RecordingHandler : public ConnectionHandler {
    template <typename F>
    auto locked(F f) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return f();
    }

public:
    void on_connect(ConnectionId, const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++connects_;
    }
    void on_frame(ConnectionId id, std::string_view frame, SteadyTime) override {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_[id].emplace_back(frame);
        ++total_frames_;
    }
    void on_oversized_frame(ConnectionId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++oversized_;
    }
    void on_disconnect(ConnectionId, DisconnectReason reason) override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnects_.push_back(reason);
    }
    void on_connection_rejected(const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejected_;
    }

    std::size_t total_frames() const { return locked([&] { return total_frames_; }); }
    std::size_t connects() const { return locked([&] { return connects_; }); }
    std::size_t oversized() const { return locked([&] { return oversized_; }); }
    std::size_t rejected() const { return locked([&] { return rejected_; }); }
    std::vector<DisconnectReason> disconnects() const { return locked([&] { return disconnects_; }); }
    std::map<ConnectionId, std::vector<std::string>> frames() const {
        return locked([&] { return frames_; });
    }

private:
    mutable std::mutex mutex_;
    std::map<ConnectionId, std::vector<std::string>> frames_;
    std::size_t total_frames_ = 0;
    std::size_t connects_ = 0;
    std::size_t oversized_ = 0;
    std::size_t rejected_ = 0;
    std::vector<DisconnectReason> disconnects_;
};

class TcpServerTest : public ::testing::Test {
protected:
    void start(TcpServerConfig config = {}) {
        config.bind_address = "127.0.0.1";
        config.port = 0;
        server_ = std::make_unique<TcpServer>(config, handler_, logger_);
        server_->start();
    }

    TcpClient connect() {
        TcpClient client;
        client.connect("127.0.0.1", server_->port());
        return client;
    }

    /// True if the server closed the connection (recv returns 0 or an error).
    static bool server_closed(TcpClient& client, std::chrono::milliseconds timeout = 2000ms) {
        timeval tv{static_cast<time_t>(timeout.count() / 1000),
                   static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
        setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char byte;
        return ::recv(client.fd(), &byte, 1, 0) <= 0 && errno != EAGAIN;
    }

    Logger logger_{LogLevel::Off};
    RecordingHandler handler_;
    std::unique_ptr<TcpServer> server_;
};

}  // namespace

TEST_F(TcpServerTest, DeliversFramesFromOneClient) {
    start();
    auto client = connect();
    ASSERT_TRUE(client.send_line("one"));
    ASSERT_TRUE(client.send_line("two"));
    ASSERT_TRUE(test::wait_until([&] { return handler_.total_frames() == 2; }));
    const auto frames = handler_.frames();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.begin()->second, (std::vector<std::string>{"one", "two"}));
    EXPECT_EQ(handler_.connects(), 1u);
}

TEST_F(TcpServerTest, ReassemblesFramesSentByteByByte) {
    start();
    auto client = connect();
    const std::string stream = "{\"partial\":1}\n{\"partial\":2}\n";
    for (char c : stream) {
        ASSERT_TRUE(client.send_raw(std::string_view(&c, 1)));
        std::this_thread::sleep_for(100us);  // TCP_NODELAY: separate segments.
    }
    ASSERT_TRUE(test::wait_until([&] { return handler_.total_frames() == 2; }));
    EXPECT_EQ(handler_.frames().begin()->second,
              (std::vector<std::string>{"{\"partial\":1}", "{\"partial\":2}"}));
}

TEST_F(TcpServerTest, ManyConcurrentClientsKeepPerConnectionOrder) {
    start();
    constexpr int kClients = 16;
    constexpr int kFrames = 300;
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            TcpClient client;
            client.connect("127.0.0.1", server_->port());
            for (int i = 0; i < kFrames; ++i) {
                ASSERT_TRUE(client.send_line(std::to_string(c) + ":" + std::to_string(i)));
            }
        });
    }
    for (auto& t : threads) t.join();
    ASSERT_TRUE(test::wait_until([&] { return handler_.total_frames() == kClients * kFrames; }));

    const auto frames = handler_.frames();
    EXPECT_EQ(frames.size(), static_cast<std::size_t>(kClients));
    for (const auto& [id, list] : frames) {
        ASSERT_EQ(list.size(), static_cast<std::size_t>(kFrames));
        const std::string prefix = list.front().substr(0, list.front().find(':') + 1);
        for (int i = 0; i < kFrames; ++i) {
            EXPECT_EQ(list[static_cast<std::size_t>(i)], prefix + std::to_string(i));
        }
    }
}

TEST_F(TcpServerTest, OversizedFrameIsDiscardedAndConnectionSurvives) {
    TcpServerConfig config;
    config.max_frame_bytes = 64;
    start(config);
    auto client = connect();
    ASSERT_TRUE(client.send_line(std::string(10'000, 'x')));
    ASSERT_TRUE(client.send_line("after"));
    ASSERT_TRUE(test::wait_until([&] { return handler_.total_frames() == 1; }));
    EXPECT_EQ(handler_.oversized(), 1u);
    EXPECT_EQ(handler_.frames().begin()->second.front(), "after");
}

TEST_F(TcpServerTest, DetectsClientDisconnect) {
    start();
    {
        auto client = connect();
        ASSERT_TRUE(client.send_raw("incomplete-frame-without-newline"));
    }  // client closes
    ASSERT_TRUE(test::wait_until([&] { return handler_.disconnects().size() == 1; }));
    EXPECT_EQ(handler_.disconnects().front(), DisconnectReason::PeerClosed);
    EXPECT_EQ(handler_.total_frames(), 0u) << "a partial frame must not be delivered";
    EXPECT_TRUE(test::wait_until([&] { return server_->active_connections() == 0; }));
}

TEST_F(TcpServerTest, IdleTimeoutClosesConnection) {
    TcpServerConfig config;
    config.idle_timeout = 100ms;
    start(config);
    auto client = connect();
    EXPECT_TRUE(server_closed(client));
    ASSERT_TRUE(test::wait_until([&] { return handler_.disconnects().size() == 1; }));
    EXPECT_EQ(handler_.disconnects().front(), DisconnectReason::IdleTimeout);
}

TEST_F(TcpServerTest, RejectsConnectionsBeyondLimit) {
    TcpServerConfig config;
    config.max_connections = 2;
    start(config);
    auto a = connect();
    auto b = connect();
    ASSERT_TRUE(test::wait_until([&] { return server_->active_connections() == 2; }));
    auto c = connect();  // TCP handshake succeeds; the server then closes it.
    EXPECT_TRUE(server_closed(c));
    ASSERT_TRUE(test::wait_until([&] { return handler_.rejected() == 1; }));
    EXPECT_EQ(server_->active_connections(), 2u);

    // Capacity frees up when a client leaves.
    a.close();
    ASSERT_TRUE(test::wait_until([&] { return server_->active_connections() == 1; }));
    auto d = connect();
    ASSERT_TRUE(d.send_line("hello"));
    ASSERT_TRUE(test::wait_until([&] { return handler_.total_frames() == 1; }));
}

TEST_F(TcpServerTest, StopClosesActiveConnectionsPromptly) {
    start();
    std::vector<TcpClient> clients;
    for (int i = 0; i < 5; ++i) {
        clients.push_back(connect());
    }
    ASSERT_TRUE(test::wait_until([&] { return server_->active_connections() == 5; }));

    const auto begin = std::chrono::steady_clock::now();
    server_->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);

    const auto reasons = handler_.disconnects();
    ASSERT_EQ(reasons.size(), 5u);
    for (auto reason : reasons) {
        EXPECT_EQ(reason, DisconnectReason::ServerShutdown);
    }
    for (auto& client : clients) {
        EXPECT_TRUE(server_closed(client));
    }
    server_->stop();  // idempotent
}

TEST_F(TcpServerTest, StartFailsWhenPortInUse) {
    start();
    TcpServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = server_->port();
    RecordingHandler other_handler;
    TcpServer second(config, other_handler, logger_);
    EXPECT_THROW(second.start(), std::system_error);
}
