#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "order_book.hpp"

namespace matching_engine {

class EventLog;

using Symbol = std::string;

// Routes orders to a per-instrument OrderBook. The books are fully
// independent -- there is no cross-instrument matching -- so this is a
// dispatcher, not a second matching engine.
//
// Every operation names its symbol, cancel and modify included. That is a
// deliberate choice rather than an oversight. The alternative, a global
// order-id -> symbol index so that cancel(id) could locate its own book, has
// to stay in step with removals the books perform on their own: a resting
// order being filled, or cancelled by self-trade prevention. The engine never
// observes those, so the index would accumulate entries for orders that no
// longer exist, and duplicate-id checks against it would start rejecting ids
// that are genuinely free. Carrying the symbol keeps cancellation O(1) with
// nothing to synchronise, and it is what real protocols do -- a FIX cancel
// request carries Symbol alongside the order id for the same reason.
//
// A consequence worth stating: because an id is only ever looked up within
// one book, order ids need to be unique per symbol, not globally.
//
// Instruments are registered up front rather than created on first use. Lazy
// creation would turn a typo'd symbol into a silently-opened new instrument
// instead of a rejected order.
//
// Not thread-safe, for the same reason OrderBook isn't. Per-symbol books do
// make the obvious sharding possible -- one thread per instrument, no shared
// state between them -- but that would be a change to this class, not a
// property it already has.
class MatchingEngine {
   public:
    explicit MatchingEngine(SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelOldest)
        : self_trade_policy_(self_trade_policy) {}

    // Attaches (or with nullptr, detaches) a write-ahead log. Once attached,
    // every request below is written to it BEFORE being applied -- including
    // requests that go on to be rejected, which is what makes the log a
    // complete record rather than a record of successes. The engine does not
    // own the log; it must outlive the engine.
    void setEventLog(EventLog* log) { event_log_ = log; }
    EventLog* eventLog() const { return event_log_; }

    // Registers an instrument. Returns false if it was already registered
    // (leaving the existing book untouched), or if the symbol is empty or
    // contains whitespace -- a symbol has to stay a single token to survive a
    // round trip through the event log or any line-based protocol.
    bool addSymbol(const Symbol& symbol);
    bool hasSymbol(const Symbol& symbol) const;
    std::size_t symbolCount() const { return books_.size(); }

    // Registered symbols, sorted. Sorted rather than in hash order because a
    // snapshot written from this has to be byte-identical for identical state;
    // unordered_map iteration order is not something to serialise.
    std::vector<Symbol> symbols() const;

    // The book for a symbol, or nullptr. Exposed for snapshotting; callers on
    // the order-entry path should use the routing methods above.
    const OrderBook* book(const Symbol& symbol) const { return findBook(symbol); }

    // Each of these forwards to that symbol's book, or is rejected with
    // RejectReason::UnknownSymbol if no such instrument is registered.
    SubmitResult addLimitOrder(const Symbol& symbol, OrderId id, Side side, Price price,
                               Quantity quantity, ParticipantId participant = kNoParticipant);
    SubmitResult addMarketOrder(const Symbol& symbol, OrderId id, Side side, Quantity quantity,
                                ParticipantId participant = kNoParticipant);
    SubmitResult modifyOrder(const Symbol& symbol, OrderId id, Price new_price, Quantity new_quantity);

    // False if the symbol is unknown or the order isn't resting in it.
    bool cancelOrder(const Symbol& symbol, OrderId id);

    // Empty for an unknown symbol, exactly as for an empty book side.
    std::optional<Price> bestBid(const Symbol& symbol) const;
    std::optional<Price> bestAsk(const Symbol& symbol) const;

    // Empty for an unknown symbol, exactly as for an empty book side. See
    // OrderBook::bidLevels/askLevels for what `depth` means.
    std::vector<PriceLevel> bidLevels(const Symbol& symbol, std::size_t depth = 0) const;
    std::vector<PriceLevel> askLevels(const Symbol& symbol, std::size_t depth = 0) const;

    // Resting orders in one book (0 if unknown), or summed across every book.
    std::size_t restingOrderCount(const Symbol& symbol) const;
    std::size_t restingOrderCount() const;

   private:
    OrderBook* findBook(const Symbol& symbol);
    const OrderBook* findBook(const Symbol& symbol) const;

    std::unordered_map<Symbol, OrderBook> books_;
    SelfTradePolicy self_trade_policy_;
    EventLog* event_log_ = nullptr;
};

}  // namespace matching_engine
