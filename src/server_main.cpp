#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "event_log.hpp"
#include "matching_engine.hpp"
#include "server.hpp"
#include "snapshot.hpp"

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

    MatchingEngine engine;

    // Same recovery path as the CLI: snapshot first, then the log records
    // after it. A server that forgets its book on restart isn't persistent.
    std::unique_ptr<std::ofstream> log_file;
    std::unique_ptr<EventLog> log;

    if (!log_path.empty()) {
        const std::string snapshot_path = log_path + ".snapshot";
        std::uint64_t resume_from = 0;

        std::ifstream snapshot_in(snapshot_path);
        if (snapshot_in) {
            const SnapshotResult loaded = loadSnapshot(snapshot_in, engine);
            if (!loaded.ok) {
                std::cerr << "snapshot at " << snapshot_path << " is incomplete; refusing to start\n";
                return 1;
            }
            resume_from = loaded.next_seq;
            std::cout << "loaded snapshot: " << loaded.orders_loaded << " orders, sequence "
                       << resume_from << "\n";
        }

        ReplayResult recovered;
        std::ifstream existing(log_path);
        if (existing) {
            recovered = replay(existing, engine, resume_from);
            std::cout << "replayed " << recovered.applied << " records from " << log_path << "\n";
        }
        if (recovered.truncated) {
            std::cerr << "log damaged at line " << recovered.stopped_at_line
                       << "; refusing to append\n";
            return 1;
        }

        log_file = std::make_unique<std::ofstream>(log_path, std::ios::app);
        if (!*log_file) {
            std::cerr << "cannot open log for writing: " << log_path << "\n";
            return 1;
        }
        log = std::make_unique<EventLog>(*log_file, resume_from + recovered.applied);
        engine.setEventLog(log.get());
    }

    OrderServer server(engine);
    std::string error;
    if (!server.listenOn(port, error)) {
        std::cerr << "cannot listen on port " << port << ": " << error << "\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::cout << "listening on 127.0.0.1:" << server.boundPort();
    if (!log_path.empty()) std::cout << ", logging to " << log_path;
    std::cout << "\nCtrl-C to stop.\n";
    std::cout.flush();

    const std::uint64_t handled = server.runUntilStopped();
    std::cout << "\nstopped after handling " << handled << " requests\n";
    return 0;
}
