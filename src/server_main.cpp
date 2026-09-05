#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

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

    MatchingEngine engine;

    // Same recovery path as the CLI -- see recovery.hpp. A server that
    // forgets its book on restart isn't persistent.
    std::unique_ptr<std::ofstream> log_file;
    std::unique_ptr<EventLog> log;

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
