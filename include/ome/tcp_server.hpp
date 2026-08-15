#pragma once

// ---------------------------------------------------------------------------
// Single-threaded non-blocking TCP listener with a poll() event loop.
//
// SESSION 1.2 SCOPE: the network layer proven in isolation. A complete, valid
// NewOrder frame gets a hardcoded Ack. There is no matching engine here, no
// queue, and no second thread — those arrive in session 1.4, and wiring them
// early would mean debugging framing and concurrency at the same time.
//
// WHY poll() AND NOT A THREAD PER CONNECTION:
//   A thread per connection costs ~8 KiB of stack plus a kernel scheduling
//   entity each, and every message then crosses a thread boundary to reach the
//   single-writer book — turning a design whose whole point is one owner into
//   one with N producers. One thread doing non-blocking I/O keeps the network
//   side sequential and the handoff to the matching thread a single seam.
//
// WHY poll() AND NOT epoll/kqueue:
//   poll() is POSIX and runs unchanged on both linux and macos, which this
//   project needs. Its cost is O(n) per call over the whole fd set, versus
//   O(ready) for the others — irrelevant at tens of connections, real at tens
//   of thousands. The revisit threshold is roughly a thousand concurrent
//   connections; below that the scan is noise next to the syscall itself.
//   Session 1.7 measures whether client count actually degrades p99 here.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ome/frame_reader.hpp"
#include "ome/protocol.hpp"
#include "ome/write_buffer.hpp"

namespace ome {

struct TcpServerConfig {
    std::uint16_t port{9001};
    int backlog{128};
    std::size_t max_write_buffer{WriteBuffer::kDefaultCapacity};
    // A peer that sends a header claiming a payload and then stalls holds this
    // much memory. Capped so an idle-but-open connection cannot pin an
    // unbounded read buffer.
    std::size_t max_read_buffer{2 * protocol::kMaxPayloadSize};
    // poll() timeout. Bounded rather than infinite so the loop wakes regularly
    // for timer work (heartbeats in session 1.3) even with no socket activity.
    int poll_timeout_ms{100};
};

// Per-connection state. Non-copyable: it owns a file descriptor.
struct Connection {
    int fd{-1};
    std::uint64_t id{0};
    FrameReader reader;
    WriteBuffer writer;
    bool want_close{false};

    Connection(int f, std::uint64_t i, std::size_t write_cap)
        : fd(f), id(i), writer(write_cap) {}
};

class TcpServer {
public:
    explicit TcpServer(TcpServerConfig cfg) : cfg_(cfg) {}
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Binds and listens. Returns false with last_error() set on failure.
    [[nodiscard]] bool start();

    // Runs the event loop until stop() is called. Blocks the calling thread.
    void run();

    // Safe to call from a signal handler and from another thread.
    //
    // std::atomic, NOT volatile. volatile means "do not optimize away this
    // access" — it says nothing about atomicity or about ordering with respect
    // to other threads, and a volatile write racing a volatile read is still a
    // data race by the standard's definition. This started out volatile and
    // ThreadSanitizer flagged it immediately.
    //
    // relaxed ordering is sufficient: the flag guards no other data, so all we
    // need is that the store eventually becomes visible, not that anything else
    // is ordered around it. The loop re-reads it every poll timeout.
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    // Actual bound port. Differs from cfg_.port when port 0 was requested,
    // which is how tests bind an ephemeral port without racing each other.
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

private:
    void accept_new();
    void handle_readable(Connection& c);
    void handle_writable(Connection& c);
    void dispatch(Connection& c, const Frame& f);
    void queue(Connection& c, const std::vector<std::uint8_t>& bytes);
    void close_connection(std::size_t index);

    TcpServerConfig cfg_;
    int listen_fd_{-1};
    std::uint16_t bound_port_{0};
    // lock-free is required for signal-handler safety: a handler that blocked
    // on a mutex inside the atomic could deadlock against the interrupted thread.
    static_assert(std::atomic<bool>::is_always_lock_free, "signal handler needs a lock-free flag");
    std::atomic<bool> running_{false};
    std::uint64_t next_conn_id_{1};
    std::uint64_t acks_{0};  // stub id source; removed in 1.4
    std::vector<std::unique_ptr<Connection>> conns_;
    std::string last_error_;
};

}  // namespace ome
