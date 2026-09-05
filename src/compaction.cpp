#include "compaction.hpp"

#include <cstdio>
#include <cstdlib>

#include "snapshot.hpp"

namespace matching_engine {

namespace {

// Parses the N in "--name=N" if `arg` starts with "--name=" and the rest is
// a valid non-negative integer with nothing trailing. Returns false (leaving
// `out` untouched) for anything else, including a present-but-malformed
// value, which the caller reports as an error rather than silently keeping
// the default.
bool parseFlagValue(const std::string& arg, const std::string& name, std::uint64_t& out) {
    const std::string prefix = "--" + name + "=";
    if (arg.rfind(prefix, 0) != 0) return false;
    const std::string value = arg.substr(prefix.size());
    if (value.empty()) return false;
    for (char c : value) {
        if (c < '0' || c > '9') return false;
    }
    out = std::strtoull(value.c_str(), nullptr, 10);
    return true;
}

}  // namespace

CompactionResult compact(const MatchingEngine& engine, const std::string& snapshot_path,
                         EventLog& log, std::ofstream& log_file, const std::string& log_path) {
    CompactionResult result;
    result.sequence = log.nextSequence();

    // Write the snapshot to a temporary and rename it into place, so a crash
    // mid-write can never leave a half-written snapshot where a complete one
    // used to be.
    const std::string temp_path = snapshot_path + ".tmp";
    {
        std::ofstream snapshot_out(temp_path, std::ios::trunc);
        if (!snapshot_out) {
            result.ok = false;
            result.error = "cannot write " + temp_path;
            return result;
        }
        writeSnapshot(snapshot_out, engine, result.sequence);
    }
    if (std::rename(temp_path.c_str(), snapshot_path.c_str()) != 0) {
        result.ok = false;
        result.error = "cannot install snapshot at " + snapshot_path;
        return result;
    }

    // Only now discard the log. A crash before this point leaves the old log
    // intact, which recovery handles by skipping the records the snapshot
    // already covers (see replay()'s first_seq skip in event_log.hpp).
    log_file.close();
    log_file.open(log_path, std::ios::trunc);
    if (!log_file) {
        result.ok = false;
        result.error = "snapshot written, but the log could not be truncated";
        return result;
    }
    return result;
}

bool AutoCompactor::shouldCompact(std::uint64_t current_sequence) const {
    if (policy_.max_records.has_value() &&
        current_sequence - last_compacted_sequence_ >= *policy_.max_records) {
        return true;
    }
    if (policy_.max_age.has_value() &&
        std::chrono::steady_clock::now() - last_compacted_at_ >= *policy_.max_age) {
        return true;
    }
    return false;
}

std::optional<CompactionResult> AutoCompactor::maybeCompact(const MatchingEngine& engine,
                                                             const std::string& snapshot_path,
                                                             EventLog& log, std::ofstream& log_file,
                                                             const std::string& log_path) {
    if (!shouldCompact(log.nextSequence())) return std::nullopt;

    CompactionResult result = compact(engine, snapshot_path, log, log_file, log_path);
    if (result.ok) {
        last_compacted_sequence_ = result.sequence;
        last_compacted_at_ = std::chrono::steady_clock::now();
    }
    // A failed attempt does NOT reset the clocks: leaving them alone means
    // the next check retries immediately rather than waiting out a fresh
    // interval after a transient failure (a full disk, say).
    return result;
}

CompactionArgsResult parseCompactionPolicyArgs(int argc, char** argv, int first) {
    CompactionArgsResult result;
    CompactionPolicy policy;
    bool saw_any = false;

    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        std::uint64_t value = 0;

        if (parseFlagValue(arg, "auto-compact-records", value)) {
            policy.max_records = value;
            saw_any = true;
        } else if (parseFlagValue(arg, "auto-compact-seconds", value)) {
            policy.max_age = std::chrono::seconds(value);
            saw_any = true;
        } else {
            result.ok = false;
            result.error = "unrecognised argument: " + arg;
            return result;
        }
    }

    if (saw_any) result.policy = policy;
    return result;
}

}  // namespace matching_engine
