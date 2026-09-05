#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include "event_log.hpp"
#include "matching_engine.hpp"

namespace matching_engine {

struct CompactionResult {
    bool ok = true;
    std::string error;
    std::uint64_t sequence = 0;  // the sequence compacted at; meaningful when ok
};

// Writes a snapshot for `engine` at `log.nextSequence()`, atomically installs
// it at `snapshot_path`, then truncates `log_file` for continued writing from
// that same sequence.
//
// This is the one implementation of compaction. It used to live inline in
// the CLI's COMPACT command handler; pulling it out is what let an automatic,
// policy-driven trigger (below) share it with the manual one instead of
// copying the crash-safety ordering a second time -- write the snapshot,
// install it, and only then touch the log, so a crash before installation
// leaves the old log intact and recovery just replays past what the snapshot
// already covers.
//
// `log` itself is not replaced. Its next_seq_ already equals the sequence
// being compacted at, since it is the same running counter that produced
// every record up to this point; only the underlying file is closed and
// reopened truncated, which is why the caller must keep passing the same
// EventLog and ofstream rather than constructing fresh ones.
CompactionResult compact(const MatchingEngine& engine, const std::string& snapshot_path,
                         EventLog& log, std::ofstream& log_file, const std::string& log_path);

// What triggers automatic compaction. A record-count threshold works
// anywhere a caller can check it after each applied request. A wall-clock
// threshold only makes sense somewhere with ticks independent of traffic --
// the server's idle hook (see OrderServer::setIdleHook) -- since a CLI
// blocked on std::getline has no "meanwhile" in which time could be checked
// between commands.
struct CompactionPolicy {
    std::optional<std::uint64_t> max_records;
    std::optional<std::chrono::steady_clock::duration> max_age;
};

// Tracks one CompactionPolicy's progress toward its thresholds and performs
// compaction when one trips. One instance per (engine, log) pair.
class AutoCompactor {
   public:
    explicit AutoCompactor(CompactionPolicy policy) : policy_(policy) {}

    // True if either threshold has been reached since the last compaction
    // (or since construction, if none has happened yet).
    bool shouldCompact(std::uint64_t current_sequence) const;

    // Compacts if shouldCompact() is true; resets both thresholds on success
    // so they measure from the new compaction point, not the old one.
    // Returns nullopt when nothing was attempted.
    std::optional<CompactionResult> maybeCompact(const MatchingEngine& engine,
                                                 const std::string& snapshot_path, EventLog& log,
                                                 std::ofstream& log_file, const std::string& log_path);

   private:
    CompactionPolicy policy_;
    std::uint64_t last_compacted_sequence_ = 0;
    std::chrono::steady_clock::time_point last_compacted_at_ = std::chrono::steady_clock::now();
};

struct CompactionArgsResult {
    bool ok = true;
    std::string error;                        // set when !ok
    std::optional<CompactionPolicy> policy;   // nullopt if neither flag was given
};

// Parses --auto-compact-records=N and --auto-compact-seconds=S out of
// argv[first..argc). Shared by the CLI and the server, which each have their
// own positional arguments before this point (a log path; a port and a log
// path) but the same trailing flags after it.
//
// An unrecognised argument in this range is reported as an error rather than
// silently skipped -- a typo'd flag should not silently disable the feature
// it was trying to enable.
CompactionArgsResult parseCompactionPolicyArgs(int argc, char** argv, int first);

}  // namespace matching_engine
