#pragma once

#include <cstdint>

namespace matching_engine {

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

using OrderId = std::uint64_t;

// Whoever the order belongs to -- the unit self-trade prevention reasons
// about. kNoParticipant is a reserved "anonymous" id that is exempt from
// self-trade prevention entirely: two anonymous orders must still be allowed
// to trade with each other, since otherwise every unattributed order would
// block every other unattributed order.
using ParticipantId = std::uint64_t;
inline constexpr ParticipantId kNoParticipant = 0;

// Prices are integer counts of ticks -- never floating point. A tick is the
// instrument's minimum price increment, kTicksPerUnit of them to one
// currency unit. This makes every representable price exact, which the book
// depends on in two places: two orders at the same price must land on the
// same map key (one price level, not two), and the crossing tests must be
// exact comparisons. With a double, `100.00` stepped up ten times by `0.01`
// is not the same value as the literal `100.10`, which is enough to split a
// level in two and leave a crossed book unmatched.
//
// Signed rather than unsigned so price arithmetic (spreads, offsets) can go
// negative without wrapping.
using Price = std::int64_t;

inline constexpr Price kTicksPerUnit = 100;  // 1 tick = 0.01 of a currency unit
inline constexpr int kTickDigits = 2;        // decimal places implied by the above

using Quantity = std::uint64_t;
using Timestamp = std::uint64_t;

// A resting or incoming order. `quantity` is mutated in place as fills
// happen, so it always reflects the remaining unfilled amount.
struct Order {
    OrderId id;
    ParticipantId participant;
    Side side;
    OrderType type;
    Price price;        // in ticks; meaningless for Market orders
    Quantity quantity;  // remaining quantity
    Timestamp timestamp;
};

// One matched fill between a resting order and an incoming order.
// The trade price (in ticks) is always the resting order's price
// (price-time priority: the order that was already in the book sets
// the price).
struct Trade {
    OrderId buy_order_id;
    OrderId sell_order_id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
};

}  // namespace matching_engine
