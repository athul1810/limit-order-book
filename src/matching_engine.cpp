#include "matching_engine.hpp"

#include <algorithm>
#include <cctype>

#include "event_log.hpp"

namespace matching_engine {

namespace {

// A symbol has to survive a round trip through the whitespace-separated event
// log, so it must be a single non-empty token.
bool isWellFormedSymbol(const Symbol& symbol) {
    if (symbol.empty()) return false;
    for (unsigned char c : symbol) {
        if (std::isspace(c)) return false;
    }
    return true;
}

}  // namespace

bool MatchingEngine::addSymbol(const Symbol& symbol) {
    if (!isWellFormedSymbol(symbol)) return false;
    if (event_log_ != nullptr) event_log_->recordAddSymbol(symbol);
    // try_emplace constructs the book in place with the engine's policy, and
    // leaves an existing entry alone.
    return books_.try_emplace(symbol, self_trade_policy_).second;
}

bool MatchingEngine::hasSymbol(const Symbol& symbol) const { return books_.count(symbol) != 0; }

std::vector<Symbol> MatchingEngine::symbols() const {
    std::vector<Symbol> names;
    names.reserve(books_.size());
    for (const auto& entry : books_) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

OrderBook* MatchingEngine::findBook(const Symbol& symbol) {
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : &it->second;
}

const OrderBook* MatchingEngine::findBook(const Symbol& symbol) const {
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : &it->second;
}

SubmitResult MatchingEngine::addLimitOrder(const Symbol& symbol, OrderId id, Side side, Price price,
                                           Quantity quantity, ParticipantId participant) {
    if (event_log_ != nullptr) {
        event_log_->recordLimitOrder(symbol, id, side, price, quantity, participant);
    }
    OrderBook* book = findBook(symbol);
    if (book == nullptr) return SubmitResult{RejectReason::UnknownSymbol, quantity, false, {}};
    return book->addLimitOrder(id, side, price, quantity, participant);
}

SubmitResult MatchingEngine::addMarketOrder(const Symbol& symbol, OrderId id, Side side,
                                            Quantity quantity, ParticipantId participant) {
    if (event_log_ != nullptr) {
        event_log_->recordMarketOrder(symbol, id, side, quantity, participant);
    }
    OrderBook* book = findBook(symbol);
    if (book == nullptr) return SubmitResult{RejectReason::UnknownSymbol, quantity, false, {}};
    return book->addMarketOrder(id, side, quantity, participant);
}

SubmitResult MatchingEngine::modifyOrder(const Symbol& symbol, OrderId id, Price new_price,
                                         Quantity new_quantity) {
    if (event_log_ != nullptr) event_log_->recordModifyOrder(symbol, id, new_price, new_quantity);
    OrderBook* book = findBook(symbol);
    if (book == nullptr) return SubmitResult{RejectReason::UnknownSymbol, new_quantity, false, {}};
    return book->modifyOrder(id, new_price, new_quantity);
}

bool MatchingEngine::cancelOrder(const Symbol& symbol, OrderId id) {
    if (event_log_ != nullptr) event_log_->recordCancelOrder(symbol, id);
    OrderBook* book = findBook(symbol);
    return (book != nullptr) && book->cancelOrder(id);
}

std::optional<Price> MatchingEngine::bestBid(const Symbol& symbol) const {
    const OrderBook* book = findBook(symbol);
    return (book == nullptr) ? std::nullopt : book->bestBid();
}

std::optional<Price> MatchingEngine::bestAsk(const Symbol& symbol) const {
    const OrderBook* book = findBook(symbol);
    return (book == nullptr) ? std::nullopt : book->bestAsk();
}

std::size_t MatchingEngine::restingOrderCount(const Symbol& symbol) const {
    const OrderBook* book = findBook(symbol);
    return (book == nullptr) ? 0 : book->restingOrderCount();
}

std::size_t MatchingEngine::restingOrderCount() const {
    std::size_t total = 0;
    for (const auto& entry : books_) total += entry.second.restingOrderCount();
    return total;
}

}  // namespace matching_engine
