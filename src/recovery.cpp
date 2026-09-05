#include "recovery.hpp"

#include "snapshot.hpp"

namespace matching_engine {

RecoveredLog recoverAndOpenLog(const std::string& log_path, MatchingEngine& engine) {
    RecoveredLog result;
    const std::string snapshot_path = log_path + ".snapshot";

    // Recovery is snapshot first, then the log records after it.
    std::uint64_t resume_from = 0;
    std::ifstream snapshot_in(snapshot_path);
    if (snapshot_in) {
        const SnapshotResult loaded = loadSnapshot(snapshot_in, engine);
        if (!loaded.ok) {
            result.ok = false;
            result.error = "snapshot at " + snapshot_path +
                            " is incomplete or unreadable; refusing to start from it.";
            return result;
        }
        resume_from = loaded.next_seq;
        result.snapshot_present = true;
        result.snapshot_orders_loaded = loaded.orders_loaded;
        result.snapshot_next_seq = resume_from;
    }

    ReplayResult recovered;
    std::ifstream existing(log_path);
    if (existing) {
        result.log_present = true;
        recovered = replay(existing, engine, resume_from);
    }
    result.records_replayed = recovered.applied;

    if (recovered.truncated) {
        // The torn bytes are still in the file. Appending past them would
        // produce a log that stops replaying at the same point forever,
        // quietly losing everything written from here on.
        result.ok = false;
        result.error = "log damaged at line " + std::to_string(recovered.stopped_at_line) +
                        "; refusing to append. Truncate it to the last intact record first.";
        return result;
    }

    result.file = std::make_unique<std::ofstream>(log_path, std::ios::app);
    if (!*result.file) {
        result.ok = false;
        result.error = "cannot open log for writing: " + log_path;
        return result;
    }
    // Continue the sequence rather than restarting it at zero. With a
    // snapshot in play that means the snapshot's sequence plus whatever the
    // log tail added.
    result.log = std::make_unique<EventLog>(*result.file, resume_from + recovered.applied);
    engine.setEventLog(result.log.get());
    return result;
}

}  // namespace matching_engine
