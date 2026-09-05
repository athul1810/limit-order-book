#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "event_log.hpp"
#include "matching_engine.hpp"
#include "snapshot.hpp"

using namespace matching_engine;

namespace {

// The two conversion points between human decimals and the engine's ticks.
// They live here, at the I/O boundary, because the book itself is strictly
// integer -- see Price in order.hpp.

// "100.5" -> 10050 ticks. Rounds to the nearest tick rather than truncating,
// so a price that isn't on the tick grid lands on the closest one instead of
// silently drifting down.
Price parsePrice(double units) {
    return static_cast<Price>(std::llround(units * static_cast<double>(kTicksPerUnit)));
}

// 10050 ticks -> "100.50". Integer-only, so display never rounds either.
std::string formatPrice(Price ticks) {
    const bool negative = ticks < 0;
    const Price magnitude = negative ? -ticks : ticks;
    std::string frac = std::to_string(magnitude % kTicksPerUnit);
    frac.insert(0, static_cast<std::size_t>(kTickDigits) - frac.size(), '0');
    return (negative ? "-" : "") + std::to_string(magnitude / kTicksPerUnit) + "." + frac;
}

// Participant is an optional trailing token. Absent (or unparseable) means
// anonymous, which is exempt from self-trade prevention.
ParticipantId readParticipant(std::istringstream& iss) {
    ParticipantId participant = kNoParticipant;
    if (!(iss >> participant)) return kNoParticipant;
    return participant;
}

void printTrades(const std::vector<Trade>& trades) {
    for (const auto& t : trades) {
        std::cout << "  TRADE buy=" << t.buy_order_id << " sell=" << t.sell_order_id
                   << " price=" << formatPrice(t.price) << " qty=" << t.quantity << "\n";
    }
}

void printBook(const MatchingEngine& engine, const Symbol& symbol) {
    auto bid = engine.bestBid(symbol);
    auto ask = engine.bestAsk(symbol);
    std::cout << "  " << symbol << ": bid=" << (bid ? formatPrice(*bid) : "-")
               << " ask=" << (ask ? formatPrice(*ask) : "-")
               << " resting=" << engine.restingOrderCount(symbol) << "\n";
}

// A rejected request left the book untouched, so there is nothing to report
// but the reason. `remainder_label` names what became of any unfilled
// quantity, which differs by order type: a limit order rests its remainder,
// a market order throws it away.
void printResult(const SubmitResult& result, const MatchingEngine& engine, const Symbol& symbol,
                 const char* remainder_label) {
    if (!result.accepted()) {
        std::cout << "  REJECTED: " << describe(result.reason) << "\n";
        return;
    }
    printTrades(result.trades);
    if (result.cancelled_by_self_trade) {
        std::cout << "  self-trade prevented: " << result.unfilled << " cancelled\n";
    } else if (result.unfilled > 0) {
        std::cout << "  " << remainder_label << " " << result.unfilled << "\n";
    }
    printBook(engine, symbol);
}

}  // namespace

int main(int argc, char** argv) {
    MatchingEngine engine;

    // With a log path, the CLI is durable across restarts: replay what's
    // already there, then append everything new to the same file.
    std::unique_ptr<std::ofstream> log_file;
    std::unique_ptr<EventLog> log;

    std::string log_path;
    std::string snapshot_path;

    if (argc > 1) {
        log_path = argv[1];
        snapshot_path = log_path + ".snapshot";
        const std::string& path = log_path;

        // Recovery is snapshot first, then the log records after it.
        std::uint64_t resume_from = 0;
        std::ifstream snapshot_in(snapshot_path);
        if (snapshot_in) {
            const SnapshotResult loaded = loadSnapshot(snapshot_in, engine);
            if (!loaded.ok) {
                std::cerr << "snapshot at " << snapshot_path
                           << " is incomplete or unreadable; refusing to start from it.\n";
                return 1;
            }
            resume_from = loaded.next_seq;
            std::cout << "loaded snapshot: " << loaded.orders_loaded << " orders, sequence "
                       << resume_from << "\n";
        }

        ReplayResult recovered;
        std::ifstream existing(path);
        if (existing) {
            recovered = replay(existing, engine, resume_from);
            std::cout << "replayed " << recovered.applied << " records from " << path << "\n";
        }

        if (recovered.truncated) {
            // The torn bytes are still in the file. Appending past them would
            // produce a log that stops replaying at the same point forever,
            // quietly losing everything written from here on.
            std::cerr << "log damaged at line " << recovered.stopped_at_line
                       << "; refusing to append. Truncate it to the last intact record first.\n";
            return 1;
        }

        log_file = std::make_unique<std::ofstream>(path, std::ios::app);
        if (!*log_file) {
            std::cerr << "cannot open log for writing: " << path << "\n";
            return 1;
        }
        // Continue the sequence rather than restarting it at zero. With a
        // snapshot in play that means the snapshot's sequence plus whatever
        // the log tail added.
        log = std::make_unique<EventLog>(*log_file, resume_from + recovered.applied);
        engine.setEventLog(log.get());
    }

    std::string line;

    std::cout << "matching-engine CLI\n"
                 "  SYMBOL <symbol>\n"
                 "  LIMIT  <symbol> BUY|SELL <id> <price> <qty> [participant]\n"
                 "  MARKET <symbol> BUY|SELL <id> <qty> [participant]\n"
                 "  MODIFY <symbol> <id> <new price> <new qty>\n"
                 "  CANCEL <symbol> <id>\n"
                 "  BOOK   <symbol>\n"
                 "  COMPACT\n"
                 "  QUIT\n"
                 "\nRun with a file path to persist: ./matching_engine_cli book.log\n"
                 "Instruments must be registered with SYMBOL before they take orders.\n"
                 "Participant is optional; omitting it means anonymous, which is exempt\n"
                 "from self-trade prevention. Order ids are unique per symbol.\n\n";

    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd.empty()) continue;
        if (cmd == "QUIT") break;

        if (cmd == "SYMBOL") {
            Symbol symbol;
            iss >> symbol;
            std::cout << "  " << symbol << (engine.addSymbol(symbol) ? " registered" : " already registered")
                       << "\n";
        } else if (cmd == "LIMIT") {
            Symbol symbol;
            std::string side_str;
            OrderId id;
            double price_units;
            Quantity qty;
            iss >> symbol >> side_str >> id >> price_units >> qty;
            Side side = (side_str == "BUY") ? Side::Buy : Side::Sell;
            printResult(engine.addLimitOrder(symbol, id, side, parsePrice(price_units), qty,
                                             readParticipant(iss)),
                        engine, symbol, "rested");
        } else if (cmd == "MARKET") {
            Symbol symbol;
            std::string side_str;
            OrderId id;
            Quantity qty;
            iss >> symbol >> side_str >> id >> qty;
            Side side = (side_str == "BUY") ? Side::Buy : Side::Sell;
            printResult(engine.addMarketOrder(symbol, id, side, qty, readParticipant(iss)), engine,
                        symbol, "discarded");
        } else if (cmd == "MODIFY") {
            Symbol symbol;
            OrderId id;
            double price_units;
            Quantity qty;
            iss >> symbol >> id >> price_units >> qty;
            printResult(engine.modifyOrder(symbol, id, parsePrice(price_units), qty), engine, symbol,
                        "rested");
        } else if (cmd == "CANCEL") {
            Symbol symbol;
            OrderId id;
            iss >> symbol >> id;
            std::cout << "  cancel " << (engine.cancelOrder(symbol, id) ? "ok" : "not found") << "\n";
        } else if (cmd == "COMPACT") {
            if (log == nullptr) {
                std::cout << "  not logging; start with a log path to enable compaction\n";
                continue;
            }
            const std::uint64_t at = log->nextSequence();

            // Write the snapshot to a temporary and rename it into place, so a
            // crash mid-write can never leave a half-written snapshot where a
            // complete one used to be.
            const std::string temp_path = snapshot_path + ".tmp";
            {
                std::ofstream snapshot_out(temp_path, std::ios::trunc);
                if (!snapshot_out) {
                    std::cout << "  cannot write " << temp_path << "\n";
                    continue;
                }
                writeSnapshot(snapshot_out, engine, at);
            }
            if (std::rename(temp_path.c_str(), snapshot_path.c_str()) != 0) {
                std::cout << "  cannot install snapshot at " << snapshot_path << "\n";
                continue;
            }

            // Only now discard the log. A crash before this point leaves the
            // old log intact, which recovery handles by skipping the records
            // the snapshot already covers.
            log_file->close();
            log_file->open(log_path, std::ios::trunc);
            if (!*log_file) {
                std::cout << "  snapshot written, but the log could not be truncated\n";
                continue;
            }
            std::cout << "  compacted at sequence " << at << ": snapshot " << snapshot_path
                       << ", log truncated\n";
        } else if (cmd == "BOOK") {
            Symbol symbol;
            iss >> symbol;
            printBook(engine, symbol);
        } else {
            std::cout << "  unknown command: " << cmd << "\n";
        }
    }

    return 0;
}
