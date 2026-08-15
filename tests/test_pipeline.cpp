// The full pipeline: sockets -> SPSC queue -> matching thread -> egress -> sockets.
//
// This is the file that proves the architecture, so it runs the REAL matching
// thread rather than a stub. Run it under -DOME_SANITIZE=thread: the whole
// point of the design is that the only shared state is the two queues, and a
// TSan-clean run over cross-session trading is the evidence.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ome/commands.hpp"
#include "ome/matching_thread.hpp"
#include "ome/protocol.hpp"
#include "ome/tcp_server.hpp"
#include "ome/waiter.hpp"

using namespace ome;
using namespace ome::protocol;

namespace {

// Server + matching thread, the production shape.
class Gateway {
public:
    Gateway() {
        matcher_ = std::make_unique<MatchingThread>(inbound_, waiter_);
        TcpServerConfig cfg{};
        cfg.port = 0;
        cfg.poll_timeout_ms = 5;
        server_ = std::make_unique<TcpServer>(cfg, &inbound_, &waiter_);
        started_ = server_->start();
        if (started_) {
            port_ = server_->bound_port();
            matcher_->start();
            net_ = std::thread([this] { server_->run(); });
        }
    }

    ~Gateway() {
        if (started_) {
            server_->stop();
            if (net_.joinable()) net_.join();
            matcher_->stop();
        }
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    InboundQueue inbound_;
    Waiter waiter_;
    std::unique_ptr<MatchingThread> matcher_;
    std::unique_ptr<TcpServer> server_;
    std::thread net_;
    bool started_{false};
    std::uint16_t port_{0};
};

int dial(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 5;
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

struct Msg {
    MessageHeader header;
    std::vector<std::uint8_t> payload;
};

bool next_msg(int fd, Msg& m) {
    std::uint8_t hb[kHeaderSize];
    if (!read_exactly(fd, hb, kHeaderSize)) return false;
    const auto h = decode_header(hb, kHeaderSize);
    if (!h.has_value()) return false;
    m.header = *h;
    m.payload.resize(h->length);
    return h->length == 0 || read_exactly(fd, m.payload.data(), h->length);
}

// Reads until a message of the wanted type arrives, skipping heartbeats.
bool next_of_type(int fd, MessageType want, Msg& m, int budget = 40) {
    for (int i = 0; i < budget; ++i) {
        if (!next_msg(fd, m)) return false;
        if (static_cast<MessageType>(m.header.type) == want) return true;
    }
    return false;
}

void send_order(int fd, std::uint64_t coid, std::int64_t px, std::uint32_t qty, Side side,
                OrderType type = OrderType::Limit) {
    NewOrder m{};
    m.client_order_id = coid;
    m.price_ticks = px;
    m.quantity = qty;
    m.side = side;
    m.order_type = type;
    const auto f = encode_frame(MessageType::NewOrder, m);
    ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));
}

}  // namespace

TEST(Pipeline, order_reaches_the_engine_and_the_ack_comes_back) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 1, 1000000, 10, Side::Bid);

    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const auto ack = decode<Ack>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->client_order_id, 1u);
    // The exchange id is assigned by the MATCHING thread, not fabricated on the
    // network side as it was in 1.2/1.3.
    EXPECT_GT(ack->exchange_order_id, 0u);
    ::close(fd);
}

TEST(Pipeline, cross_session_fill_routes_to_both_sides) {
    // THE test for the ownership map. A Trade carries order ids, not session
    // ids; without owner_of_ there is no way to decide which socket each side
    // of a fill belongs to.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    // maker rests an ask
    send_order(maker, 100, 1000000, 10, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));

    // taker crosses it
    send_order(taker, 200, 1000000, 4, Side::Bid);
    ASSERT_TRUE(next_of_type(taker, MessageType::Ack, m));
    EXPECT_EQ(decode<Ack>(m.payload.data(), m.payload.size())->client_order_id, 200u);

    // taker's fill: fully filled, nothing remaining
    ASSERT_TRUE(next_of_type(taker, MessageType::Fill, m)) << "taker never got a fill";
    const auto tf = decode<Fill>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->price_ticks, 1000000);
    EXPECT_EQ(tf->quantity, 4u);
    EXPECT_EQ(tf->remaining_quantity, 0u);

    // maker's fill: same trade, but 6 of its 10 still rest
    ASSERT_TRUE(next_of_type(maker, MessageType::Fill, m)) << "maker never got a fill";
    const auto mf = decode<Fill>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(mf.has_value());
    EXPECT_EQ(mf->price_ticks, 1000000);
    EXPECT_EQ(mf->quantity, 4u);
    EXPECT_EQ(mf->remaining_quantity, 6u) << "maker's remaining quantity is wrong";

    ::close(maker);
    ::close(taker);
}

TEST(Pipeline, ack_arrives_before_fill) {
    // Protocol commitment: the engine rests an aggressive limit and then matches
    // it, so a marketable order is acked before it is filled. Clients model
    // their own state on this ordering.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    send_order(maker, 1, 1000000, 5, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));

    send_order(taker, 2, 1000000, 5, Side::Bid);

    // first non-heartbeat message on the taker must be the Ack
    std::vector<MessageType> seen;
    for (int i = 0; i < 20 && seen.size() < 2; ++i) {
        ASSERT_TRUE(next_msg(taker, m));
        const auto t = static_cast<MessageType>(m.header.type);
        if (t == MessageType::Heartbeat) continue;
        seen.push_back(t);
    }
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], MessageType::Ack) << "Fill preceded Ack";
    EXPECT_EQ(seen[1], MessageType::Fill);

    ::close(maker);
    ::close(taker);
}

TEST(Pipeline, cancel_is_decided_by_the_book_not_the_network_thread) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 7, 1000000, 10, Side::Bid);
    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));

    const auto c = encode_frame(MessageType::Cancel, Cancel{7});
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    EXPECT_EQ(decode<Ack>(m.payload.data(), m.payload.size())->client_order_id, 7u);

    // cancelling it again must be rejected by the engine
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));
    ASSERT_TRUE(next_of_type(fd, MessageType::Reject, m));
    EXPECT_EQ(decode<Reject>(m.payload.data(), m.payload.size())->reason,
              RejectReason::UNKNOWN_ORDER);
    ::close(fd);
}

TEST(Pipeline, unknown_cancel_is_rejected_by_the_engine) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    const auto c = encode_frame(MessageType::Cancel, Cancel{999999});
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));

    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Reject, m));
    EXPECT_EQ(decode<Reject>(m.payload.data(), m.payload.size())->reason,
              RejectReason::UNKNOWN_ORDER);
    ::close(fd);
}

TEST(Pipeline, modify_loses_priority_and_gets_a_new_exchange_id) {
    // Modify is cancel + new applied as a unit on the matching thread. The new
    // exchange_order_id is the observable evidence that queue position moved,
    // which docs/PROTOCOL.md commits to.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 11, 1000000, 10, Side::Bid);
    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const std::uint64_t first_id = decode<Ack>(m.payload.data(), m.payload.size())->exchange_order_id;

    Modify mod{};
    mod.client_order_id = 11;
    mod.new_price_ticks = 1010000;
    mod.new_quantity = 10;
    const auto f = encode_frame(MessageType::Modify, mod);
    ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));

    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const auto ack = decode<Ack>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->client_order_id, 11u);
    EXPECT_NE(ack->exchange_order_id, first_id) << "reprice kept its exchange id";
    ::close(fd);
}

TEST(Pipeline, disconnect_cancels_resting_orders_through_the_command_path) {
    // Cancel-on-disconnect, for real this time: the stub is gone and the
    // cancels are performed by the matching thread from a CancelAllForSession
    // command travelling the same queue as everything else.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    ASSERT_GE(maker, 0);
    send_order(maker, 1, 1000000, 10, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));
    ::close(maker);  // vanish while holding a resting ask

    // Give the command time to be applied.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // A new session crossing the old price must find nothing: the book is empty.
    const int taker = dial(g.port());
    ASSERT_GE(taker, 0);
    send_order(taker, 2, 1000000, 10, Side::Bid);
    ASSERT_TRUE(next_of_type(taker, MessageType::Ack, m));

    // No fill should follow. Read a few messages; heartbeats are fine, a Fill
    // means the dead session's order was still resting.
    for (int i = 0; i < 3; ++i) {
        Msg extra{};
        if (!next_msg(taker, extra)) break;
        EXPECT_NE(static_cast<MessageType>(extra.header.type), MessageType::Fill)
            << "a disconnected session's order was still matchable";
    }
    ::close(taker);
}

TEST(Pipeline, sustained_two_way_trading_stays_consistent) {
    // Volume, so TSan has real interleavings to inspect rather than a single
    // tidy handoff. Every taker order crosses a resting maker order, so the
    // fill count is deterministic even though the timing is not.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    constexpr int kRounds = 200;
    std::atomic<int> maker_fills{0};

    // Drain the maker's socket concurrently, so its egress queue keeps moving
    // and the network thread is genuinely interleaved with the matcher.
    std::thread maker_reader([&] {
        Msg m{};
        while (maker_fills.load() < kRounds) {
            if (!next_msg(maker, m)) return;
            if (static_cast<MessageType>(m.header.type) == MessageType::Fill) {
                maker_fills.fetch_add(1);
            }
        }
    });

    for (int i = 0; i < kRounds; ++i) {
        const auto coid = static_cast<std::uint64_t>(i + 1);
        send_order(maker, coid, 1000000, 1, Side::Ask);
        send_order(taker, coid + 100000, 1000000, 1, Side::Bid);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (maker_fills.load() < kRounds && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    maker_reader.join();

    EXPECT_EQ(maker_fills.load(), kRounds) << "lost fills under sustained load";
    ::close(maker);
    ::close(taker);
}
