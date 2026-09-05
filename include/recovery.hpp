#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "event_log.hpp"
#include "matching_engine.hpp"

namespace matching_engine {

// Recovers `engine` from a log file and, if one exists, its companion
// snapshot, then opens the log for continued append-only writing and
// attaches it to `engine`.
//
// This used to be ~50 lines duplicated verbatim in the CLI and the server --
// load the snapshot if present, replay the log from the snapshot's sequence,
// refuse to proceed past a truncated log, then open the log in append mode
// starting from the right sequence number. Two independent copies of that
// meant the two entry points could silently drift: a fix applied to one
// (say, the sequence-continuation-on-append logic) would leave the other
// wrong until someone noticed. There is exactly one implementation now.
struct RecoveredLog {
    // False means recovery cannot proceed safely: a snapshot that failed its
    // completeness check, a log torn by a mid-write crash, or a log file that
    // could not be (re)opened for writing. `engine` may be partially
    // populated in this case and must not be used further; the caller should
    // print `error` and exit.
    bool ok = true;
    std::string error;

    bool snapshot_present = false;
    std::uint64_t snapshot_orders_loaded = 0;
    std::uint64_t snapshot_next_seq = 0;  // meaningful only if snapshot_present

    // False on a first run against a path with no log file yet -- there was
    // nothing to replay, as distinct from a log that existed but was empty.
    // Callers use this to decide whether a "replayed N records" line belongs
    // in their startup banner at all.
    bool log_present = false;
    std::uint64_t records_replayed = 0;

    // Owns the open log file and the EventLog already attached to `engine`
    // via setEventLog(). Both must outlive `engine`; on success, keeping this
    // struct alive is what keeps the engine logging.
    std::unique_ptr<std::ofstream> file;
    std::unique_ptr<EventLog> log;
};

RecoveredLog recoverAndOpenLog(const std::string& log_path, MatchingEngine& engine);

}  // namespace matching_engine
