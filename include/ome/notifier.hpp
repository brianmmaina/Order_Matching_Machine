#pragma once

// ---------------------------------------------------------------------------
// Wakes the NETWORK thread when the matching thread has produced events.
//
// The mirror image of Waiter. Waiter parks the matching thread until commands
// arrive; Notifier hands the network thread a file descriptor it can poll
// alongside its sockets, so a freshly produced Ack goes out on this iteration
// rather than whenever the loop next happens to wake.
//
// WHY THIS EXISTS: the loop drains egress queues before it reads sockets, so an
// ack generated from this iteration's reads was not sent until the following
// pass — and that pass only ran when another client happened to send something
// or the poll timeout expired. Reordering alone does not fix it, because the
// matching thread is asynchronous and the ack may not exist yet at any fixed
// point in the loop.
//
// The load generator measured the result as a p50 of 5.1ms against a real cost
// of tens of microseconds: almost all of it was the gateway waiting to be told
// there was something to send.
//
// Different shape from Waiter deliberately. The network thread already blocks
// in poll() on its sockets and must not block anywhere else, so this exposes a
// descriptor to add to that poll set rather than a wait() call of its own.
// ---------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

namespace ome {

class Notifier {
public:
    Notifier() {
        if (::pipe(fds_) != 0) {
            fds_[0] = fds_[1] = -1;
            return;
        }
        // Both ends non-blocking. The read end especially: drain() reads until
        // empty, and on a blocking descriptor that final read parks the network
        // thread forever — the same bug that cost real time in Waiter.
        set_nonblocking(fds_[0]);
        set_nonblocking(fds_[1]);
    }

    ~Notifier() {
        if (fds_[0] >= 0) ::close(fds_[0]);
        if (fds_[1] >= 0) ::close(fds_[1]);
    }

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fds_[0] >= 0 && fds_[1] >= 0; }
    [[nodiscard]] int poll_fd() const noexcept { return fds_[0]; }

    // Matching thread. Coalescing: one pending byte means "look at the egress
    // queues", and a second byte would say nothing more. Skipping the write
    // when a wake-up is already outstanding keeps a burst of a thousand fills
    // from costing a thousand syscalls on the hot path.
    void notify() {
        if (pending_.exchange(true, std::memory_order_release)) {
            return;
        }
        const std::uint8_t byte = 1;
        const ssize_t r = ::write(fds_[1], &byte, 1);
        static_cast<void>(r);
    }

    // Network thread, after poll() reports the descriptor readable.
    //
    // Clear AFTER draining, not before. Clearing first looks safer and is not:
    // a notify() landing between the clear and the read writes a byte, this
    // read loop consumes it, and the channel is left with pending_ == true and
    // an EMPTY pipe. Every subsequent notify() then skips its write forever,
    // and the network thread only ever wakes on its poll timeout. That is a
    // permanently dead notification channel, not a missed wake-up, and it cost
    // real debugging time here — it showed up as a p99 in the tens of
    // milliseconds while p50 stayed under 200us.
    //
    // Clearing afterwards still leaves a one-event window (a notify during the
    // read may be consumed and then cleared), which is why the caller must NOT
    // depend on this for correctness. The network loop independently checks
    // whether any egress queue is non-empty before it blocks; this channel is a
    // latency optimisation, not the mechanism that guarantees delivery.
    void drain() {
        std::uint8_t buf[64];
        while (::read(fds_[0], buf, sizeof(buf)) > 0) {
        }
        pending_.store(false, std::memory_order_release);
    }

private:
    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            static_cast<void>(::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
        }
    }

    int fds_[2]{-1, -1};
    std::atomic<bool> pending_{false};
};

}  // namespace ome
