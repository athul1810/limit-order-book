#include "snapshot.hpp"

#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "event_log.hpp"
#include "order_book.hpp"

namespace matching_engine {

namespace {

const char* sideToken(Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

bool parseSide(const std::string& token, Side& out) {
    if (token == "BUY") {
        out = Side::Buy;
        return true;
    }
    if (token == "SELL") {
        out = Side::Sell;
        return true;
    }
    return false;
}

}  // namespace

void writeSnapshot(std::ostream& out, const MatchingEngine& engine, std::uint64_t next_seq) {
    out << "SNAPSHOT " << kSnapshotFormatVersion << " " << next_seq << "\n";

    const std::vector<Symbol> names = engine.symbols();

    // Symbols first, and all of them: a registered instrument with an empty
    // book has no ORDER lines to imply its existence.
    for (const Symbol& name : names) out << "SYMBOL " << name << "\n";

    for (const Symbol& name : names) {
        const OrderBook* book = engine.book(name);
        if (book == nullptr) continue;
        for (const Order& order : book->restingOrders()) {
            out << "ORDER " << name << " " << order.id << " " << sideToken(order.side) << " "
                << order.price << " " << order.quantity << " " << order.participant << "\n";
        }
    }

    out << "END\n";
    out.flush();
}

SnapshotResult loadSnapshot(std::istream& in, MatchingEngine& engine) {
    EventLog* saved_log = engine.eventLog();
    engine.setEventLog(nullptr);

    SnapshotResult result;
    std::string line;
    bool saw_header = false;
    bool saw_end = false;

    while (std::getline(in, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string kind;
        iss >> kind;

        if (kind == "SNAPSHOT") {
            int version = 0;
            if (!(iss >> version >> result.next_seq) || version != kSnapshotFormatVersion) break;
            saw_header = true;
        } else if (!saw_header) {
            break;  // anything before the header means this isn't a snapshot
        } else if (kind == "SYMBOL") {
            Symbol name;
            if (!(iss >> name) || !engine.addSymbol(name)) break;
        } else if (kind == "ORDER") {
            Symbol name;
            OrderId id = 0;
            std::string side_token;
            Price price = 0;
            Quantity quantity = 0;
            ParticipantId participant = kNoParticipant;
            Side side = Side::Buy;
            if (!(iss >> name >> id >> side_token >> price >> quantity >> participant) ||
                !parseSide(side_token, side)) {
                break;
            }
            // Re-inserting in snapshot order rebuilds each level's queue. These
            // cannot trade on the way in: a resting book is never crossed, so
            // no bid in it can cross any ask in it.
            if (!engine.addLimitOrder(name, id, side, price, quantity, participant).accepted()) break;
            ++result.orders_loaded;
        } else if (kind == "END") {
            saw_end = true;
            break;
        } else {
            break;  // unrecognised record
        }
    }

    result.ok = saw_header && saw_end;
    engine.setEventLog(saved_log);
    return result;
}

}  // namespace matching_engine
