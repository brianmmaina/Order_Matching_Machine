#pragma once

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3 polynomial, reflected 0xEDB88320).
//
// Used to detect a torn or corrupted WAL record. A crash can leave the last
// record half-written — the process died between the write() and the next one,
// or the page cache flushed part of it — and recovery has to be able to say
// "this record is not intact" rather than replaying garbage into the book.
//
// A checksum is the only way to know. Record length alone is not enough: a
// torn write can produce a plausible length field followed by nothing, or a
// complete-looking record whose payload is half old data from a reused block.
//
// CRC32 rather than a cryptographic hash because the threat is accidental
// corruption, not an adversary editing the log. It detects all single-bit
// errors, all burst errors up to 32 bits, and the overwhelming majority of
// everything else, at roughly a byte per cycle. A SHA would be slower for no
// benefit against the failure mode that actually occurs.
//
// Table-driven and computed once at first use. Standard-library only, matching
// the project's dependency rule.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>

namespace ome {

namespace detail {

inline std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            // reflected polynomial: shift right, xor in 0xEDB88320 when the
            // low bit was set.
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

inline const std::array<std::uint32_t, 256>& crc32_table() {
    static const std::array<std::uint32_t, 256> table = make_crc32_table();
    return table;
}

}  // namespace detail

// Streaming form: pass the previous result back in as `crc` to continue over
// several buffers. Start with 0.
[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size,
                                         std::uint32_t crc = 0) {
    const auto& table = detail::crc32_table();
    // The standard pre- and post-inversion. Without it, leading zero bytes
    // would not affect the result and a record of zeros would checksum the
    // same as an empty one — exactly the pattern a torn write leaves behind.
    std::uint32_t c = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace ome
