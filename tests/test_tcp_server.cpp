// In-process integration test: a real listener on a real socket.
//
// The framing tests drive FrameReader/WriteBuffer directly, which is where the
// boundary conditions are checked. This file checks the part those cannot: that
// the poll loop, the socket options, and the dispatch path are actually wired
// together, and that hostile input reaches a clean rejection rather than a
// hang or a crash.
//
// Binds port 0 so the OS picks a free port — tests must not race each other or
// fail because a developer happens to have something on 9001.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "ome/protocol.hpp"
#include "ome/tcp_server.hpp"

using namespace ome;
using namespace ome::protocol;

namespace {

// Starts a server on its own thread and tears it down on destruction.
class ServerFixture {
public:
    ServerFixture() {
        TcpServerConfig cfg{};
        cfg.port = 0;               // ephemeral
        cfg.poll_timeout_ms = 10;   // keep stop() responsive
        server_ = std::make_unique<TcpServer>(cfg);
        started_ = server_->start();
        if (started_) {
            port_ = server_->bound_port();
            thread_ = std::thread([this] { server_->run(); });
        }
    }

    ~ServerFixture() {
        if (started_) {
            server_->stop();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    std::unique_ptr<TcpServer> server_;
    std::thread thread_;
    bool started_{false};
    std::uint16_t port_{0};
};

int dial(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    // Bound so a server that never replies fails the test instead of hanging it.
    timeval tv{};
    tv.tv_sec = 3;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
    return fd;
}

bool read_exactly(int fd, std::uint8_t* out, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, out + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// Reads one frame. Returns false on timeout or peer close.
bool read_frame(int fd, MessageHeader& h, std::vector<std::uint8_t>& payload) {
    std::uint8_t hb[kHeaderSize];
    if (!read_exactly(fd, hb, kHeaderSize)) return false;
    const auto parsed = decode_header(hb, kHeaderSize);
    if (!parsed.has_value()) return false;
    h = *parsed;
    payload.resize(h.length);
    return h.length == 0 || read_exactly(fd, payload.data(), h.length);
}

std::vector<std::uint8_t> valid_new_order(std::uint64_t id, std::uint32_t qty) {
    NewOrder m{};
    m.client_order_id = id;
    m.price_ticks = 1000000;
    m.quantity = qty;
    m.side = Side::Bid;
    m.order_type = OrderType::Limit;
    return encode_frame(MessageType::NewOrder, m);
}

}  // namespace

TEST(TcpServer, binds_an_ephemeral_port_and_acks_a_valid_order) {
    ServerFixture s;
    ASSERT_TRUE(s.started());
    ASSERT_NE(s.port(), 0);

    const int fd = dial(s.port());
    ASSERT_GE(fd, 0);

    const auto frame = valid_new_order(11, 5);
    ASSERT_EQ(::send(fd, frame.data(), frame.size(), 0), static_cast<ssize_t>(frame.size()));

    MessageHeader h{};
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(read_frame(fd, h, payload));
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(MessageType::Ack));

    const auto ack = decode<Ack>(payload.data(), payload.size());
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->client_order_id, 11u);
    ::close(fd);
}

TEST(TcpServer, reassembles_a_frame_sent_one_byte_at_a_time) {
    // The end-to-end version of the FrameReader split test: real socket, real
    // segmentation, 30 separate writes for one message.
    ServerFixture s;
    ASSERT_TRUE(s.started());
    const int fd = dial(s.port());
    ASSERT_GE(fd, 0);

    const auto frame = valid_new_order(22, 7);
    for (const std::uint8_t b : frame) {
        ASSERT_EQ(::send(fd, &b, 1, 0), 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    MessageHeader h{};
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(read_frame(fd, h, payload));
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(MessageType::Ack));
    EXPECT_EQ(decode<Ack>(payload.data(), payload.size())->client_order_id, 22u);
    ::close(fd);
}

TEST(TcpServer, handles_several_frames_in_one_write) {
    ServerFixture s;
    ASSERT_TRUE(s.started());
    const int fd = dial(s.port());
    ASSERT_GE(fd, 0);

    std::vector<std::uint8_t> batch;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        const auto f = valid_new_order(i, 1);
        batch.insert(batch.end(), f.begin(), f.end());
    }
    ASSERT_EQ(::send(fd, batch.data(), batch.size(), 0), static_cast<ssize_t>(batch.size()));

    for (std::uint64_t i = 1; i <= 4; ++i) {
        MessageHeader h{};
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(read_frame(fd, h, payload)) << "no ack for order " << i;
        EXPECT_EQ(decode<Ack>(payload.data(), payload.size())->client_order_id, i);
    }
    ::close(fd);
}

TEST(TcpServer, rejects_zero_quantity_with_a_reason) {
    ServerFixture s;
    ASSERT_TRUE(s.started());
    const int fd = dial(s.port());
    ASSERT_GE(fd, 0);

    const auto frame = valid_new_order(33, 0);
    ASSERT_EQ(::send(fd, frame.data(), frame.size(), 0), static_cast<ssize_t>(frame.size()));

    MessageHeader h{};
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(read_frame(fd, h, payload));
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(MessageType::Reject));
    const auto rej = decode<Reject>(payload.data(), payload.size());
    ASSERT_TRUE(rej.has_value());
    EXPECT_EQ(rej->reason, RejectReason::INVALID_QTY);
    ::close(fd);
}

TEST(TcpServer, rejects_unknown_message_type_and_bad_version) {
    ServerFixture s;
    ASSERT_TRUE(s.started());

    {
        const int fd = dial(s.port());
        ASSERT_GE(fd, 0);
        std::vector<std::uint8_t> f;
        MessageHeader h{};
        h.length = 0;
        h.type = 4242;
        h.version = kVersion;
        encode_header(f, h);
        ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));

        MessageHeader rh{};
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(read_frame(fd, rh, payload));
        EXPECT_EQ(rh.type, static_cast<std::uint16_t>(MessageType::Reject));
        EXPECT_EQ(decode<Reject>(payload.data(), payload.size())->reason,
                  RejectReason::UNKNOWN_MESSAGE_TYPE);
        ::close(fd);
    }
    {
        const int fd = dial(s.port());
        ASSERT_GE(fd, 0);
        auto f = valid_new_order(1, 1);
        f[6] = 99;  // version field, low byte
        ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));

        MessageHeader rh{};
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(read_frame(fd, rh, payload));
        EXPECT_EQ(rh.type, static_cast<std::uint16_t>(MessageType::Reject));
        EXPECT_EQ(decode<Reject>(payload.data(), payload.size())->reason, RejectReason::MALFORMED);
        ::close(fd);
    }
}

TEST(TcpServer, survives_abrupt_disconnects_garbage_and_churn) {
    // The adversarial set, automated. Each of these once required a manual
    // script; leaving them manual means they stop being run.
    ServerFixture s;
    ASSERT_TRUE(s.started());

    // half a frame, then vanish
    {
        const int fd = dial(s.port());
        ASSERT_GE(fd, 0);
        const auto frame = valid_new_order(1, 1);
        ASSERT_GT(::send(fd, frame.data(), 5, 0), 0);
        ::close(fd);
    }
    // pure garbage: decodes to an absurd length, so the stream fails and the
    // connection is dropped. That is the accepted cost of delimiter-free framing.
    {
        const int fd = dial(s.port());
        ASSERT_GE(fd, 0);
        const std::vector<std::uint8_t> junk(64, 0xFF);
        ASSERT_GT(::send(fd, junk.data(), junk.size(), 0), 0);
        ::close(fd);
    }
    // connect/close churn — catches fd leaks
    for (int i = 0; i < 100; ++i) {
        const int fd = dial(s.port());
        ASSERT_GE(fd, 0) << "connect failed at iteration " << i << " (fd exhaustion?)";
        ::close(fd);
    }

    // still correct after all of it
    const int fd = dial(s.port());
    ASSERT_GE(fd, 0);
    const auto frame = valid_new_order(777, 3);
    ASSERT_EQ(::send(fd, frame.data(), frame.size(), 0), static_cast<ssize_t>(frame.size()));
    MessageHeader h{};
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(read_frame(fd, h, payload));
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(MessageType::Ack));
    EXPECT_EQ(decode<Ack>(payload.data(), payload.size())->client_order_id, 777u);
    ::close(fd);
}
