#pragma once

// ---------------------------------------------------------------------------
// Price representation.
//
// Prices are int64 TICKS everywhere inside the engine. One tick is 1/10000 of
// a currency unit, matching the LOBSTER file convention — column 5 of a LOBSTER
// message file is already an integer tick count, so the ingest path performs no
// conversion at all.
//
// Why not double:
//   - Exchanges match on exact price equality. Two orders are at the same level
//     or they are not; there is no epsilon that makes that question meaningful,
//     and any epsilon you pick is wrong at some price scale.
//   - Session 2.2 rebuilds the book from a write-ahead log and asserts the
//     recovered book's digest is EQUAL to the original. A digest over doubles
//     turns a durability guarantee into a float-equality claim.
//   - 0.1 + 0.2 != 0.3. A price ladder built by repeated addition drifts.
//
// int64 ticks span roughly +/-9.2e14 currency units at this scale, several
// orders of magnitude beyond any instrument that will trade here.
//
// There are deliberately NO decimal<->tick conversion helpers here. Nothing in
// the codebase currently converts: LOBSTER supplies ticks, the book stores
// ticks, the JSONL snapshot format carries ticks, and tools/book_replay.html
// does the one division that exists, for display, in JavaScript. Helpers get
// added when something actually needs them — an unused conversion function is
// an invitation to convert somewhere that should not.
//
// If you add one, it belongs at a BOUNDARY: parsing human input, or formatting
// human output. A call from inside the matching path is a bug, because it means
// a price round-tripped through a representation that cannot hold it exactly.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ome {

// Mirrored by TICKS_PER_UNIT in tools/book_replay.html. If this changes, that
// changes, or every price the visualizer renders is silently wrong by 10^n.
inline constexpr std::int64_t kTicksPerUnit = 10000;

}  // namespace ome
