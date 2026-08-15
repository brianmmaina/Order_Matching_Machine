#pragma once

// ---------------------------------------------------------------------------
// Outbound byte buffer for one connection, with a hard cap.
//
// THE THING THIS CLASS EXISTS FOR: send() does not have to send everything.
// On a non-blocking socket it writes what fits in the kernel's send buffer and
// returns a short count, or fails with EAGAIN when nothing fits. Code that
// assumes send() is all-or-nothing silently truncates messages the moment a
// peer reads slowly — producing a corrupt stream rather than a clean error.
//
// So unsent bytes have to be held until the socket is writable again, which
// raises the question this class answers: how much do we hold?
//
// SLOW-CONSUMER POLICY: bounded, and disconnect on overflow.
//
// Unbounded buffering is a denial-of-service vector wearing a helpful hat. A
// client that connects, subscribes, and then simply stops calling read() will
// make the server queue every message it ever produces for that session. Memory
// grows without limit until the process is killed — and it is the SERVER that
// dies, not the misbehaving client. Worse, it is not necessarily malicious: a
// client that hits a GC pause or a slow disk looks identical.
//
// Bounding it forces an explicit choice about what to do when the bound is hit,
// and for ORDER FLOW the only correct answer is to disconnect. Acks, rejects
// and fills are not idempotent state updates — each one is a distinct fact
// about the client's position that it cannot reconstruct. Dropping one silently
// leaves the client believing something false about its own orders, which is
// worse than telling it plainly that it fell behind. Cancel-on-disconnect
// (session 1.3) then cleans up its resting orders.
//
// MARKET DATA IS DIFFERENT and gets the opposite policy in session 1.6: a
// BookUpdate is a full snapshot, so a newer one makes an older one irrelevant
// and the right move is conflation, not disconnection. Being able to say why
// those two streams differ is the point.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ome {

class WriteBuffer {
public:
    static constexpr std::size_t kDefaultCapacity = 1024 * 1024;  // 1 MiB per connection

    explicit WriteBuffer(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    // Appends bytes. Returns false if the append would exceed the cap, in which
    // case NOTHING is appended — a partial append would put a half-message on
    // the wire, which is strictly worse than refusing. The caller disconnects.
    [[nodiscard]] bool append(const std::uint8_t* data, std::size_t size) {
        if (size == 0) {
            return true;
        }
        if (pending() + size > capacity_) {
            return false;
        }
        buf_.insert(buf_.end(), data, data + size);
        return true;
    }

    [[nodiscard]] bool append(const std::vector<std::uint8_t>& bytes) {
        return append(bytes.data(), bytes.size());
    }

    [[nodiscard]] bool empty() const noexcept { return pending() == 0; }
    [[nodiscard]] std::size_t pending() const noexcept { return buf_.size() - pos_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Contiguous unsent region — hand straight to send().
    [[nodiscard]] const std::uint8_t* data() const noexcept { return buf_.data() + pos_; }

    // Call with send()'s return value. Advancing an index rather than erasing
    // keeps a partial write O(1); the buffer is compacted only when drained or
    // when the consumed prefix gets large.
    void consume(std::size_t n) {
        pos_ += std::min(n, pending());
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        } else if (pos_ >= kCompactThreshold) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
    }

private:
    static constexpr std::size_t kCompactThreshold = 64 * 1024;

    std::vector<std::uint8_t> buf_;
    std::size_t pos_{0};
    std::size_t capacity_;
};

}  // namespace ome
