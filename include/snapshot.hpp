#pragma once

#include <cstdint>
#include <iosfwd>

#include "matching_engine.hpp"

namespace matching_engine {

// Point-in-time snapshot of engine state, the other half of log compaction.
//
// The event log alone is a complete record, but recovery time grows with total
// history and the file never shrinks. A snapshot bounds both: it captures the
// state as of sequence number N, so recovery becomes "load the snapshot, then
// replay only records from N onward" and everything before N can be discarded.
//
// What has to be captured is narrower than it looks. Resting orders need their
// full identity AND their queue position, since time priority is not derivable
// from anything else. Registered symbols need capturing separately from their
// orders, because an instrument with an empty book is still a registered
// instrument. Nothing else is state: matched trades are outputs, not something
// the engine holds.
//
// Format, one record per line, prices in ticks:
//   SNAPSHOT <format version> <next sequence>
//   SYMBOL <name>
//   ORDER <symbol> <id> <BUY|SELL> <price> <quantity> <participant>
//   END
// ORDER lines appear in the exact sequence they must be re-inserted in; the
// trailing END is what distinguishes a complete snapshot from one truncated by
// a crash mid-write.
inline constexpr int kSnapshotFormatVersion = 1;

// Writes `engine` as of sequence `next_seq` -- the sequence number the next
// logged request will carry, i.e. every request numbered below it is already
// reflected in this snapshot.
void writeSnapshot(std::ostream& out, const MatchingEngine& engine, std::uint64_t next_seq);

struct SnapshotResult {
    // False if the stream was empty, malformed, a version this build doesn't
    // understand, or missing its END marker. `engine` is then left partially
    // populated and must not be used -- load into a fresh engine and discard it
    // on failure.
    bool ok = false;
    std::uint64_t next_seq = 0;  // resume replaying the log from here
    std::uint64_t orders_loaded = 0;
};

// Loads a snapshot into `engine`, which should be freshly constructed.
//
// As with replay(), any attached event log is detached for the duration:
// re-logging a snapshot restore would write the whole book back onto the log
// that compaction just shortened.
SnapshotResult loadSnapshot(std::istream& in, MatchingEngine& engine);

}  // namespace matching_engine
