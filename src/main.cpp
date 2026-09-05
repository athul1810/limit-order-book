#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "compaction.hpp"
#include "event_log.hpp"
#include "matching_engine.hpp"
#include "recovery.hpp"

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
    // already there, then append everything new to the same file. Recovery
    // itself is shared with the server -- see recovery.hpp.
    std::unique_ptr<std::ofstream> log_file;
    std::unique_ptr<EventLog> log;

    std::string log_path;
    std::string snapshot_path;
    std::unique_ptr<AutoCompactor> auto_compactor;

    if (argc > 1) {
        log_path = argv[1];
        snapshot_path = log_path + ".snapshot";

        // Trailing flags, if any: --auto-compact-records=N and/or
        // --auto-compact-seconds=S. A wall-clock trigger only fires here
        // when another command actually arrives to check it against -- the
        // CLI has no periodic tick of its own while blocked on stdin.
        const CompactionArgsResult compaction_args = parseCompactionPolicyArgs(argc, argv, 2);
        if (!compaction_args.ok) {
            std::cerr << compaction_args.error << "\n";
            return 1;
        }
        if (compaction_args.policy.has_value()) {
            auto_compactor = std::make_unique<AutoCompactor>(*compaction_args.policy);
        }

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
                 "Add --auto-compact-records=N and/or --auto-compact-seconds=S to compact\n"
                 "automatically instead of only on an explicit COMPACT.\n"
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
            const CompactionResult result = compact(engine, snapshot_path, *log, *log_file, log_path);
            if (!result.ok) {
                std::cout << "  " << result.error << "\n";
                continue;
            }
            std::cout << "  compacted at sequence " << result.sequence << ": snapshot " << snapshot_path
                       << ", log truncated\n";
        } else if (cmd == "BOOK") {
            Symbol symbol;
            iss >> symbol;
            printBook(engine, symbol);
        } else {
            std::cout << "  unknown command: " << cmd << "\n";
        }

        // Checked after every command, since this is the only "meanwhile"
        // the CLI has -- there is no tick between commands while it is
        // blocked on the next std::getline.
        if (auto_compactor != nullptr && log != nullptr) {
            const auto result = auto_compactor->maybeCompact(engine, snapshot_path, *log, *log_file,
                                                              log_path);
            if (result.has_value()) {
                if (result->ok) {
                    std::cout << "  auto-compacted at sequence " << result->sequence << "\n";
                } else {
                    std::cout << "  auto-compaction failed: " << result->error << "\n";
                }
            }
        }
    }

    return 0;
}
