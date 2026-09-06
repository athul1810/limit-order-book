#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "compaction.hpp"
#include "event_log.hpp"
#include "matching_engine.hpp"
#include "recovery.hpp"
#include "server.hpp"

using namespace matching_engine;

namespace {

// The handler may only touch this, and only through stop(), which is a single
// atomic store. Anything more inside a signal handler is undefined behaviour.
OrderServer* g_server = nullptr;

void handleSignal(int) {
    if (g_server != nullptr) g_server->stop();
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 9001;
    std::string log_path;

    if (argc > 1) port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    if (argc > 2) log_path = argv[2];

    // Trailing flags after the log path: --auto-compact-records=N and/or
    // --auto-compact-seconds=S.
    const CompactionArgsResult compaction_args = parseCompactionPolicyArgs(argc, argv, 3);
    if (!compaction_args.ok) {
        std::cerr << compaction_args.error << "\n";
        return 1;
    }
    std::unique_ptr<AutoCompactor> auto_compactor;
    if (compaction_args.policy.has_value()) {
        auto_compactor = std::make_unique<AutoCompactor>(*compaction_args.policy);
    }

    // Both come from the environment, not argv: a secret has no business
    // showing up in `ps`, and keeping the bind address alongside it means
    // the one thing that gates leaving loopback (a configured token) lives
    // right next to the setting it gates, with no interaction with the
    // trailing --auto-compact-* flags above.
    const char* token_env = std::getenv("MATCHING_ENGINE_TOKEN");
    const std::optional<std::string> required_token =
        (token_env != nullptr && *token_env != '\0') ? std::optional<std::string>(token_env)
                                                       : std::nullopt;

    const char* bind_env = std::getenv("MATCHING_ENGINE_BIND");
    const std::string bind_address =
        (bind_env != nullptr && *bind_env != '\0') ? std::string(bind_env) : "127.0.0.1";

    // Both optional, both fall back to OrderServer's own defaults when unset
    // or unparsable -- a malformed override should not be able to disable
    // rate limiting outright by accident.
    std::uint32_t max_auth_failures = OrderServer::kDefaultMaxAuthFailures;
    if (const char* max_failures_env = std::getenv("MATCHING_ENGINE_MAX_AUTH_FAILURES");
        max_failures_env != nullptr && *max_failures_env != '\0') {
        const long parsed = std::atol(max_failures_env);
        if (parsed > 0) max_auth_failures = static_cast<std::uint32_t>(parsed);
    }
    std::chrono::steady_clock::duration auth_lockout_duration = OrderServer::kDefaultAuthLockoutDuration;
    if (const char* lockout_env = std::getenv("MATCHING_ENGINE_AUTH_LOCKOUT_SECONDS");
        lockout_env != nullptr && *lockout_env != '\0') {
        const long parsed = std::atol(lockout_env);
        if (parsed > 0) auth_lockout_duration = std::chrono::seconds(parsed);
    }

    // How many aggregated price levels per side every MarketData message
    // carries. 0 would mean "the whole book" (see OrderBook::bidLevels), not
    // used as this default for exactly that reason: an unbounded depth times
    // every subscribed connection is an unbounded push size.
    std::size_t market_data_depth = OrderServer::kDefaultMarketDataDepth;
    if (const char* depth_env = std::getenv("MATCHING_ENGINE_MARKET_DATA_DEPTH");
        depth_env != nullptr && *depth_env != '\0') {
        const long parsed = std::atol(depth_env);
        if (parsed > 0) market_data_depth = static_cast<std::size_t>(parsed);
    }

    // How many past MarketData pushes per symbol ResyncMarketData can still
    // hand back verbatim before falling back to a fresh snapshot.
    std::size_t market_data_history_limit = OrderServer::kDefaultMarketDataHistoryLimit;
    if (const char* history_env = std::getenv("MATCHING_ENGINE_MARKET_DATA_HISTORY_LIMIT");
        history_env != nullptr && *history_env != '\0') {
        const long parsed = std::atol(history_env);
        if (parsed > 0) market_data_history_limit = static_cast<std::size_t>(parsed);
    }

    MatchingEngine engine;

    // Same recovery path as the CLI -- see recovery.hpp. A server that
    // forgets its book on restart isn't persistent.
    std::unique_ptr<std::ofstream> log_file;
    std::unique_ptr<EventLog> log;
    const std::string snapshot_path = log_path + ".snapshot";

    if (!log_path.empty()) {
        RecoveredLog recovered = recoverAndOpenLog(log_path, engine);
        if (!recovered.ok) {
            std::cerr << recovered.error << "\n";
            return 1;
        }
        if (recovered.snapshot_present) {
            std::cout << "loaded snapshot: " << recovered.snapshot_orders_loaded << " orders, sequence "
                       << recovered.snapshot_next_seq << "\n";
        }
        if (recovered.log_present) {
            std::cout << "replayed " << recovered.records_replayed << " records from " << log_path
                       << "\n";
        }

        log_file = std::move(recovered.file);
        log = std::move(recovered.log);
    }

    OrderServer server(engine, required_token, max_auth_failures, auth_lockout_duration, market_data_depth,
                       market_data_history_limit);

    // Unlike the CLI, the server has a genuine idle tick (the poll loop's
    // own timeout) independent of request traffic, so a wall-clock trigger
    // here actually fires on a quiet server instead of only when a request
    // happens to arrive.
    if (auto_compactor != nullptr && log != nullptr) {
        server.setIdleHook([&]() {
            const auto result =
                auto_compactor->maybeCompact(engine, snapshot_path, *log, *log_file, log_path);
            if (!result.has_value()) return;
            if (result->ok) {
                std::cout << "auto-compacted at sequence " << result->sequence << "\n";
            } else {
                std::cerr << "auto-compaction failed: " << result->error << "\n";
            }
        });
    }

    std::string error;
    if (!server.listenOn(port, error, bind_address)) {
        std::cerr << "cannot listen on " << bind_address << ":" << port << ": " << error << "\n";
        // OrderServer's message is deliberately caller-agnostic, so the
        // concrete fix (an env var, specific to this binary) is added here.
        if (!required_token.has_value()) std::cerr << "set MATCHING_ENGINE_TOKEN to allow it.\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::cout << "listening on " << bind_address << ":" << server.boundPort();
    if (required_token.has_value()) std::cout << " (authentication required)";
    if (!log_path.empty()) std::cout << ", logging to " << log_path;
    std::cout << "\nCtrl-C to stop.\n";
    std::cout.flush();

    const std::uint64_t handled = server.runUntilStopped();
    std::cout << "\nstopped after handling " << handled << " requests\n";
    return 0;
}
