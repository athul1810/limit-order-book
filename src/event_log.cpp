#include "event_log.hpp"

#include <istream>
#include <ostream>
#include <sstream>
#include <string>

namespace matching_engine {

namespace {

const char* sideToken(Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

// Returns false for anything that isn't a side token, so a corrupt record is
// rejected rather than silently read as a buy.
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

void EventLog::recordAddSymbol(const Symbol& symbol) {
    out_ << next_seq_++ << " SYMBOL " << symbol << "\n";
    out_.flush();
}

void EventLog::recordLimitOrder(const Symbol& symbol, OrderId id, Side side, Price price,
                                Quantity quantity, ParticipantId participant) {
    out_ << next_seq_++ << " LIMIT " << symbol << " " << id << " " << sideToken(side) << " " << price
         << " " << quantity << " " << participant << "\n";
    out_.flush();
}

void EventLog::recordMarketOrder(const Symbol& symbol, OrderId id, Side side, Quantity quantity,
                                 ParticipantId participant) {
    out_ << next_seq_++ << " MARKET " << symbol << " " << id << " " << sideToken(side) << " "
         << quantity << " " << participant << "\n";
    out_.flush();
}

void EventLog::recordModifyOrder(const Symbol& symbol, OrderId id, Price new_price,
                                 Quantity new_quantity) {
    out_ << next_seq_++ << " MODIFY " << symbol << " " << id << " " << new_price << " "
         << new_quantity << "\n";
    out_.flush();
}

void EventLog::recordCancelOrder(const Symbol& symbol, OrderId id) {
    out_ << next_seq_++ << " CANCEL " << symbol << " " << id << "\n";
    out_.flush();
}

ReplayResult replay(std::istream& in, MatchingEngine& engine, std::uint64_t first_seq) {
    // Detach the log so replayed records aren't written straight back onto it.
    EventLog* saved_log = engine.eventLog();
    engine.setEventLog(nullptr);

    ReplayResult result;
    std::uint64_t expected_seq = first_seq;
    std::uint64_t line_number = 0;
    std::string line;

    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::uint64_t seq = 0;
        std::string kind;
        if (!(iss >> seq >> kind)) {
            result.truncated = true;
            result.stopped_at_line = line_number;
            break;
        }
        // Already reflected in the snapshot we were loaded from.
        if (seq < first_seq) continue;
        if (seq != expected_seq) {
            result.truncated = true;
            result.stopped_at_line = line_number;
            break;
        }

        Symbol symbol;
        bool ok = false;

        if (kind == "SYMBOL") {
            ok = static_cast<bool>(iss >> symbol);
            if (ok) engine.addSymbol(symbol);
        } else if (kind == "LIMIT") {
            OrderId id = 0;
            std::string side_token;
            Price price = 0;
            Quantity quantity = 0;
            ParticipantId participant = kNoParticipant;
            Side side = Side::Buy;
            ok = static_cast<bool>(iss >> symbol >> id >> side_token >> price >> quantity >> participant) &&
                 parseSide(side_token, side);
            if (ok) engine.addLimitOrder(symbol, id, side, price, quantity, participant);
        } else if (kind == "MARKET") {
            OrderId id = 0;
            std::string side_token;
            Quantity quantity = 0;
            ParticipantId participant = kNoParticipant;
            Side side = Side::Buy;
            ok = static_cast<bool>(iss >> symbol >> id >> side_token >> quantity >> participant) &&
                 parseSide(side_token, side);
            if (ok) engine.addMarketOrder(symbol, id, side, quantity, participant);
        } else if (kind == "MODIFY") {
            OrderId id = 0;
            Price price = 0;
            Quantity quantity = 0;
            ok = static_cast<bool>(iss >> symbol >> id >> price >> quantity);
            if (ok) engine.modifyOrder(symbol, id, price, quantity);
        } else if (kind == "CANCEL") {
            OrderId id = 0;
            ok = static_cast<bool>(iss >> symbol >> id);
            if (ok) engine.cancelOrder(symbol, id);
        }

        if (!ok) {
            result.truncated = true;
            result.stopped_at_line = line_number;
            break;
        }

        ++result.applied;
        ++expected_seq;
    }

    engine.setEventLog(saved_log);
    return result;
}

}  // namespace matching_engine
