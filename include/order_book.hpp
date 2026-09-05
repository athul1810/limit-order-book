#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order.hpp"

namespace matching_engine {

// What to do when an incoming order would trade against a resting order
// belonging to the same participant. Every venue has to pick something here:
// letting the trade happen produces a wash trade, which is generally either
// prohibited or reportable, so the engine has to break the match instead --
// and breaking it necessarily means cancelling one of the two orders.
enum class SelfTradePolicy {
    // Cancel the resting order and let the incoming one keep matching into
    // whatever is behind it. Favours the aggressor; the participant loses the
    // queue position they had already earned.
    CancelOldest,
    // Cancel the incoming order at the point of the self-match. Its unfilled
    // remainder does NOT rest -- resting it would leave the participant sitting
    // crossed against their own book. Favours the resting order.
    CancelNewest,
};

// Why a request was turned away. A rejected request leaves the book
// completely untouched.
enum class RejectReason {
    None,  // accepted -- note this still permits zero trades: it didn't cross
    // An order with this id is already resting. Admitting it would strand one
    // of the two, since the id->location map can only point at one.
    DuplicateOrderId,
    // Modify named an order that isn't resting: never existed, already filled,
    // or already cancelled.
    UnknownOrder,
    // A zero-quantity modify. Cancelling is what cancelOrder is for; treating
    // this as a cancel would make a typo silently destructive.
    InvalidQuantity,
    // Engine-level only: no book is registered for that symbol.
    UnknownSymbol,
    // Wire-level only, never produced by OrderBook or MatchingEngine
    // themselves: the connection has not authenticated yet, and the server
    // requires it to before anything else. See OrderServer in server.hpp.
    NotAuthenticated,
    // Wire-level only: an Authenticate attempt presented the wrong token.
    AuthenticationFailed,
};

// Human-readable form of a RejectReason, for logs and CLI output.
const char* describe(RejectReason reason);

// The outcome of submitting or modifying an order.
//
// `reason` says whether the request was accepted and, if not, why. Note that
// an accepted order may still produce no trades: it simply didn't cross.
//
// `unfilled` is the quantity this request did not match, which means two
// different things depending on how it got here, because the two order types
// dispose of a remainder differently:
//   - limit order:  the amount now resting in the book
//   - market order: the amount discarded, since market orders never rest
// On a rejection it is the full requested quantity -- nothing was filled.
// The amount that did fill is always `requested - unfilled`.
//
// `cancelled_by_self_trade` distinguishes the one case where an accepted limit
// order does not rest its remainder: it ran into its own resting order under
// SelfTradePolicy::CancelNewest and was cancelled instead. Without the flag a
// caller would see `unfilled > 0` and reasonably assume it was working in the
// book, when in fact it no longer exists.
struct SubmitResult {
    RejectReason reason;
    Quantity unfilled;
    bool cancelled_by_self_trade;
    std::vector<Trade> trades;

    bool accepted() const { return reason == RejectReason::None; }
};

// A single-instrument limit order book with price-time priority matching.
//
// Bids are kept highest-price-first, asks lowest-price-first. Within a
// price level, orders are stored in arrival order (a std::list, so
// cancellation doesn't invalidate other iterators). An incoming order
// walks the opposite side from the best price outward, consuming resting
// orders until it is filled or no more prices cross.
//
// All prices in and out of this class are in ticks (see Price in order.hpp).
// The book performs no floating-point arithmetic at all; converting to and
// from human-readable decimals is the caller's job, at the I/O boundary.
//
// Not thread-safe by design: a real engine would pin matching to a single
// thread per instrument and move concurrency to the boundary (network I/O,
// order intake queues), not into the book itself.
class OrderBook {
   public:
    explicit OrderBook(SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelOldest)
        : self_trade_policy_(self_trade_policy) {}

    // Submits a limit order. Matches immediately against the opposite side
    // while prices cross; any unfilled remainder rests in the book.
    //
    // Rejected (book untouched) if `id` belongs to an order
    // currently resting in the book. Allowing a duplicate would orphan the
    // first order: the id->location map can only point at one of them, so
    // the other becomes both uncancellable and invisible to the accounting.
    // An id that has since been cancelled or fully filled is free to reuse.
    //
    // `participant` is trailing and optional because most callers of a
    // single-instrument book don't model ownership at all; omitting it means
    // "anonymous", which is exempt from self-trade prevention (see
    // kNoParticipant in order.hpp).
    SubmitResult addLimitOrder(OrderId id, Side side, Price price, Quantity quantity,
                               ParticipantId participant = kNoParticipant);

    // Submits a market order. Matches immediately against the best
    // available prices until filled or the opposite side is empty.
    // Any unfilled remainder is discarded -- market orders never rest.
    //
    // Rejected on a duplicate id for the same reason as addLimitOrder: a
    // market order carrying the id of a resting order would trade against
    // it and erase that order's location entry out from under it.
    SubmitResult addMarketOrder(OrderId id, Side side, Quantity quantity,
                                ParticipantId participant = kNoParticipant);

    // Cancels a resting order by id. Returns false if it doesn't exist
    // (never existed, already fully filled, or already cancelled).
    bool cancelOrder(OrderId id);

    // Cancel-replace: repoints a resting order at a new price and quantity,
    // keeping its id. Rejected (book untouched) if `id` is not currently
    // resting, or if `new_quantity` is zero -- cancelling is what CancelOrder
    // is for, and treating a zero-quantity modify as a cancel would make a
    // typo silently destructive.
    //
    // Time priority follows the usual exchange convention:
    //   - reducing quantity at an unchanged price KEEPS queue position, since
    //     giving up size never costs you your place in line
    //   - any price change, or any increase in quantity, SURRENDERS it: the
    //     order goes to the back of its new price level as a fresh arrival
    //
    // Because a surrendering modify re-enters the book as a new arrival, it
    // also matches: repricing aggressively enough to cross will trade
    // immediately, and the returned trades reflect that.
    // The participant is inherited from the order being modified -- a modify
    // cannot change who owns an order.
    SubmitResult modifyOrder(OrderId id, Price new_price, Quantity new_quantity);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

    // Number of resting orders across both sides. Handy for tests/benchmarks.
    std::size_t restingOrderCount() const { return locations_.size(); }

    // Every resting order, bids first then asks, each side from the best price
    // outward and in queue order within a level -- exactly the order matching
    // would consume them, and therefore the order they must be re-inserted in
    // to rebuild the book with its time priority intact.
    //
    // Allocates a copy of the whole book, so this is a cold-path API: snapshots
    // and diagnostics, not anything on the order-entry path.
    std::vector<Order> restingOrders() const;

   private:
    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // The incoming order as it works its way through the book. Bundled into
    // one struct because matching needs to both read it (who owns it, how
    // aggressive it is) and write back to it (what's left, whether self-trade
    // prevention killed it), and threading six parameters through two nearly
    // identical functions obscured which of them were outputs.
    //
    // is_limit=false means "market order": ignore limit_price and keep
    // consuming levels until remaining hits 0 or the book side is empty.
    struct Incoming {
        OrderId id;
        ParticipantId participant;
        Quantity remaining;
        Price limit_price;
        bool is_limit;
        bool cancelled_by_self_trade = false;
    };

    std::vector<Trade> matchBuyAgainstAsks(Incoming& incoming);
    std::vector<Trade> matchSellAgainstBids(Incoming& incoming);

    bool isSelfTrade(ParticipantId resting, ParticipantId incoming) const;

    void rest(Side side, OrderId id, Price price, Quantity quantity, ParticipantId participant);

    std::map<Price, std::list<Order>, std::greater<Price>> bids_;  // best bid first
    std::map<Price, std::list<Order>> asks_;                       // best ask first
    std::unordered_map<OrderId, Location> locations_;
    Timestamp clock_ = 0;  // monotonically increasing logical clock
    SelfTradePolicy self_trade_policy_;
};

}  // namespace matching_engine
