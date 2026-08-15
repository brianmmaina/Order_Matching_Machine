#pragma once

// ---------------------------------------------------------------------------
// How the matching thread sleeps when there is nothing to do.
//
// POLICY: bounded spin, then block on a self-pipe.
//
//   busy   -> the queue is rarely empty, the spin almost always finds work,
//             and no syscall is made at all
//   idle   -> the spin expires, the consumer parks, and the CPU goes to zero
//
// The alternatives and why not:
//   pure spin   - lowest, most predictable latency and no wake-up race to get
//                 wrong, but burns a core continuously at zero load. Hard to
//                 defend for a service that is idle most of the time.
//   pure block  - every command pays a wake-up syscall on both sides, putting
//                 a floor of several microseconds under p50.
//
// THE RACE THIS CLASS EXISTS TO GET RIGHT
//
// The obvious implementation is broken:
//
//     if (queue.empty()) { block(); }        // consumer
//     queue.push(x); if (parked) signal();   // producer
//
// The consumer can observe empty, and the producer can push AND observe
// not-parked, both before the consumer actually parks. No signal is sent, the
// consumer blocks, and the command sits in the queue until something unrelated
// wakes it. Under light load that is a message stalled indefinitely — the kind
// of bug that never reproduces on a busy machine.
//
// The fix is to publish the intent to park BEFORE the final emptiness check:
//
//     consumer:  parked = true                 (seq_cst)
//                if (!queue.empty()) { parked = false; continue; }
//                block()
//     producer:  push()                        (release)
//                if (parked) signal()          (seq_cst load)
//
// Both sides need sequential consistency across the store/load pair. With only
// acquire/release, the consumer's store to `parked` and its subsequent load of
// the queue state can be reordered against the producer's push and its load of
// `parked` — StoreLoad is the one ordering that release/acquire does not give
// you, and it is exactly the one at issue here. Getting this wrong produces a
// hang that is rare, load-dependent, and essentially undebuggable after the
// fact, so it is worth the fence.
//
// A spurious wake-up is harmless: the consumer just finds the queue empty and
// spins again. A MISSED wake-up is not. Every ambiguity here is resolved in
// favor of waking too often.
// ---------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

namespace ome {

class Waiter {
public:
    Waiter() {
        // A self-pipe rather than eventfd: eventfd is Linux-only and this must
        // build on macOS too. The cost is one extra descriptor.
        if (::pipe(fds_) != 0) {
            fds_[0] = fds_[1] = -1;
            return;
        }
        // BOTH ends must be non-blocking.
        //
        // Write end: if the pipe buffer is full there are already unread
        // wake-ups pending, so dropping ours is correct rather than blocking
        // the producer — which is the network thread serving every session.
        //
        // Read end: drain() reads until the pipe is empty, and the last read
        // finds nothing. On a BLOCKING read end that final call parks the
        // matching thread forever — it stops draining commands entirely, while
        // looking perfectly alive. That was a real bug here, and the symptom
        // was maximally confusing: the queue filled, the producer reported
        // success, and the consumer simply never ran again.
        set_nonblocking(fds_[0]);
        set_nonblocking(fds_[1]);
    }

    ~Waiter() {
        if (fds_[0] >= 0) ::close(fds_[0]);
        if (fds_[1] >= 0) ::close(fds_[1]);
    }

    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fds_[0] >= 0 && fds_[1] >= 0; }

    // Producer side. Call AFTER the push is published.
    //
    // The parked check keeps the common case free: while the consumer is
    // spinning it is not parked, so a busy gateway never touches the pipe.
    void signal() {
        // seq_cst: must not be reordered before the queue push, or we can read
        // a stale "not parked" for a consumer that has already committed to
        // blocking on the command we just enqueued.
        if (parked_.load(std::memory_order_seq_cst)) {
            const std::uint8_t byte = 1;
            // EAGAIN means the pipe already holds unread wake-ups. Ignoring the
            // result is deliberate; there is nothing useful to do and the
            // consumer is going to wake regardless.
            const ssize_t r = ::write(fds_[1], &byte, 1);
            static_cast<void>(r);
        }
    }

    // Consumer side. `has_work` is re-polled at each stage; it must be cheap
    // and must not consume anything.
    //
    // Returns when has_work() is true, or when the block times out.
    template <typename HasWork>
    void wait(HasWork has_work, int spin_iterations = 2000, int timeout_ms = 50) {
        for (int i = 0; i < spin_iterations; ++i) {
            if (has_work()) {
                return;
            }
            cpu_relax();
        }

        // Publish the intent to park BEFORE the final check. See the header
        // comment: reversing these two lines is the missed-wake-up bug.
        parked_.store(true, std::memory_order_seq_cst);

        if (has_work()) {
            parked_.store(false, std::memory_order_seq_cst);
            return;
        }

        block(timeout_ms);
        parked_.store(false, std::memory_order_seq_cst);
        drain();
    }

private:
    static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        // yield hints to SMT siblings that this core is spinning.
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            static_cast<void>(::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
        }
    }

    void block(int timeout_ms) {
        pollfd p{};
        p.fd = fds_[0];
        p.events = POLLIN;
        // A bounded timeout, not infinite. Belt and braces against a missed
        // wake-up: if the ordering above were ever wrong, this turns a hang
        // into a bounded stall. It also gives the loop a chance to notice a
        // shutdown flag.
        const int r = ::poll(&p, 1, timeout_ms);
        static_cast<void>(r);
    }

    void drain() {
        // Coalesce: many signals may have accumulated while we were awake, and
        // they all mean the same thing ("look at the queue"). Draining keeps
        // the pipe from filling and turning every future signal into an EAGAIN.
        std::uint8_t buf[256];
        for (;;) {
            const ssize_t n = ::read(fds_[0], buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            if (static_cast<std::size_t>(n) < sizeof(buf)) {
                return;
            }
        }
    }

    int fds_[2]{-1, -1};
    std::atomic<bool> parked_{false};
};

}  // namespace ome
