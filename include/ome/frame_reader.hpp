#pragma once

// ---------------------------------------------------------------------------
// Reassembles protocol frames from a TCP byte stream.
//
// THE THING THIS CLASS EXISTS FOR: one recv() is not one frame. TCP is a byte
// stream with no message boundaries. A 100-byte frame can arrive as 40 + 40 +
// 20 across three segments; three small frames can arrive in a single read;
// and a read can end halfway through a length prefix. Any code that assumes
// "one read = one message" works perfectly on localhost with small messages
// and fails in production under load or across a real network — which is
// exactly the class of bug that is miserable to diagnose after the fact.
//
// Deliberately knows nothing about sockets. Feed it bytes from anywhere; that
// is what makes it testable at every split boundary without a network.
//
// Ownership of failure: once failed() is true the connection is unrecoverable
// and must be closed. A framing error means the peer's idea of where messages
// begin no longer matches ours, and there is no way to resynchronize a stream
// that has no delimiters — which is the cost of length-prefix framing, and the
// reason the length field is validated so carefully.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ome/protocol.hpp"

namespace ome {

struct Frame {
    protocol::MessageHeader header;
    std::vector<std::uint8_t> payload;
};

class FrameReader {
public:
    // Compact the buffer once consumed bytes exceed this. Amortizes the erase:
    // without it, a long-lived connection either grows forever or memmoves the
    // whole buffer after every single frame.
    static constexpr std::size_t kCompactThreshold = 64 * 1024;

    void append(const std::uint8_t* data, std::size_t size) {
        if (failed_ || size == 0) {
            return;
        }
        buf_.insert(buf_.end(), data, data + size);
    }

    // Returns the next complete frame, or nullopt when more bytes are needed.
    // Call repeatedly after each append until it returns nullopt — a single
    // read can carry several frames, and stopping after one would leave them
    // sitting in the buffer until the peer happens to send more.
    [[nodiscard]] std::optional<Frame> next_frame() {
        if (failed_) {
            return std::nullopt;
        }

        const std::size_t available = buf_.size() - pos_;
        if (available < protocol::kHeaderSize) {
            return std::nullopt;  // not even a header yet
        }

        const auto header = protocol::decode_header(buf_.data() + pos_, available);
        if (!header.has_value()) {
            // decode_header only fails here on an oversized length claim, since
            // we already know we have kHeaderSize bytes. Unrecoverable: we
            // cannot find where the next frame starts.
            failed_ = true;
            return std::nullopt;
        }

        const std::size_t need = protocol::kHeaderSize + header->length;
        if (available < need) {
            return std::nullopt;  // header complete, payload still arriving
        }

        Frame f{};
        f.header = *header;
        const std::uint8_t* payload_begin = buf_.data() + pos_ + protocol::kHeaderSize;
        f.payload.assign(payload_begin, payload_begin + header->length);

        pos_ += need;
        compact_if_needed();
        return f;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // Bytes held but not yet formed into a frame. The server caps this: a peer
    // that opens a connection and dribbles one byte a second would otherwise
    // pin a buffer indefinitely.
    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - pos_; }

private:
    void compact_if_needed() {
        if (pos_ == buf_.size()) {
            // fully drained: the common case, and free.
            buf_.clear();
            pos_ = 0;
        } else if (pos_ >= kCompactThreshold) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
    }

    std::vector<std::uint8_t> buf_;
    std::size_t pos_{0};
    bool failed_{false};
};

}  // namespace ome
