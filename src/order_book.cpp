#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace matching_engine {

const char* describe(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "accepted";
        case RejectReason::DuplicateOrderId: return "duplicate order id (already resting in the book)";
        case RejectReason::UnknownOrder: return "no such resting order";
        case RejectReason::InvalidQuantity: return "quantity must be greater than zero";
        case RejectReason::UnknownSymbol: return "unknown symbol";
        case RejectReason::NotAuthenticated: return "not authenticated";
        case RejectReason::AuthenticationFailed: return "authentication failed";
        case RejectReason::RateLimited: return "rate limited: too many recent failed authentication attempts";
    }
    return "unknown rejection";
}

bool OrderBook::isSelfTrade(ParticipantId resting, ParticipantId incoming) const {
    // The anonymous id is never a self-match, even against itself: it means
    // "ownership not modelled", not "everyone is the same person".
    return incoming != kNoParticipant && resting == incoming;
}

std::vector<Trade> OrderBook::matchBuyAgainstAsks(Incoming& incoming) {
    std::vector<Trade> trades;

    while (incoming.remaining > 0 && !asks_.empty()) {
        auto level_it = asks_.begin();
        Price level_price = level_it->first;
        if (incoming.is_limit && level_price > incoming.limit_price) break;  // best ask too expensive

        auto& orders_at_level = level_it->second;
        while (incoming.remaining > 0 && !orders_at_level.empty()) {
            Order& resting = orders_at_level.front();

            if (isSelfTrade(resting.participant, incoming.participant)) {
                if (self_trade_policy_ == SelfTradePolicy::CancelNewest) {
                    incoming.cancelled_by_self_trade = true;
                    break;
                }
                // CancelOldest: the resting order steps aside without trading,
                // and the incoming order carries on into whatever is behind it.
                locations_.erase(resting.id);
                orders_at_level.pop_front();
                continue;
            }

            Quantity fill_qty = std::min(incoming.remaining, resting.quantity);

            trades.push_back(Trade{incoming.id, resting.id, level_price, fill_qty, ++clock_});

            incoming.remaining -= fill_qty;
            resting.quantity -= fill_qty;

            if (resting.quantity == 0) {
                locations_.erase(resting.id);
                orders_at_level.pop_front();
            }
        }

        if (orders_at_level.empty()) {
            asks_.erase(level_it);
        }
        // Unlike a filled-out order, a cancelled one still has quantity left,
        // so the outer loop needs telling explicitly to stop.
        if (incoming.cancelled_by_self_trade) break;
    }

    return trades;
}

std::vector<Trade> OrderBook::matchSellAgainstBids(Incoming& incoming) {
    std::vector<Trade> trades;

    while (incoming.remaining > 0 && !bids_.empty()) {
        auto level_it = bids_.begin();
        Price level_price = level_it->first;
        if (incoming.is_limit && level_price < incoming.limit_price) break;  // best bid too cheap

        auto& orders_at_level = level_it->second;
        while (incoming.remaining > 0 && !orders_at_level.empty()) {
            Order& resting = orders_at_level.front();

            if (isSelfTrade(resting.participant, incoming.participant)) {
                if (self_trade_policy_ == SelfTradePolicy::CancelNewest) {
                    incoming.cancelled_by_self_trade = true;
                    break;
                }
                locations_.erase(resting.id);
                orders_at_level.pop_front();
                continue;
            }

            Quantity fill_qty = std::min(incoming.remaining, resting.quantity);

            trades.push_back(Trade{resting.id, incoming.id, level_price, fill_qty, ++clock_});

            incoming.remaining -= fill_qty;
            resting.quantity -= fill_qty;

            if (resting.quantity == 0) {
                locations_.erase(resting.id);
                orders_at_level.pop_front();
            }
        }

        if (orders_at_level.empty()) {
            bids_.erase(level_it);
        }
        if (incoming.cancelled_by_self_trade) break;
    }

    return trades;
}

void OrderBook::rest(Side side, OrderId id, Price price, Quantity quantity, ParticipantId participant) {
    Order order{id, participant, side, OrderType::Limit, price, quantity, ++clock_};

    if (side == Side::Buy) {
        auto& level = bids_[price];
        level.push_back(order);
        locations_[id] = Location{side, price, std::prev(level.end())};
    } else {
        auto& level = asks_[price];
        level.push_back(order);
        locations_[id] = Location{side, price, std::prev(level.end())};
    }
}

SubmitResult OrderBook::addLimitOrder(OrderId id, Side side, Price price, Quantity quantity,
                                      ParticipantId participant) {
    // Reject before touching the book: locations_ maps one id to one resting
    // order, so admitting a second live order under the same id would strand
    // whichever one the map stops pointing at.
    if (locations_.count(id) != 0) return SubmitResult{RejectReason::DuplicateOrderId, quantity, false, {}};

    Incoming incoming{id, participant, quantity, price, true};
    std::vector<Trade> trades =
        (side == Side::Buy) ? matchBuyAgainstAsks(incoming) : matchSellAgainstBids(incoming);

    // A self-trade-cancelled order is gone: resting its remainder would leave
    // the participant crossed against their own book.
    if (incoming.remaining > 0 && !incoming.cancelled_by_self_trade) {
        rest(side, id, price, incoming.remaining, participant);
    }

    // `remaining` is now exactly what rested, or what was cancelled.
    return SubmitResult{RejectReason::None, incoming.remaining, incoming.cancelled_by_self_trade,
                        std::move(trades)};
}

SubmitResult OrderBook::addMarketOrder(OrderId id, Side side, Quantity quantity,
                                       ParticipantId participant) {
    if (locations_.count(id) != 0) return SubmitResult{RejectReason::DuplicateOrderId, quantity, false, {}};

    // limit_price is unused when is_limit=false; 0 is just a placeholder.
    Incoming incoming{id, participant, quantity, 0, false};
    std::vector<Trade> trades =
        (side == Side::Buy) ? matchBuyAgainstAsks(incoming) : matchSellAgainstBids(incoming);

    // `remaining` is now exactly what was discarded -- market orders never rest.
    return SubmitResult{RejectReason::None, incoming.remaining, incoming.cancelled_by_self_trade,
                        std::move(trades)};
}

bool OrderBook::cancelOrder(OrderId id) {
    auto loc_it = locations_.find(id);
    if (loc_it == locations_.end()) return false;

    const Location& loc = loc_it->second;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) asks_.erase(level_it);
    }

    locations_.erase(loc_it);
    return true;
}

SubmitResult OrderBook::modifyOrder(OrderId id, Price new_price, Quantity new_quantity) {
    // A zero-quantity modify is a rejected request, not a cancel. Silently
    // treating it as one would turn a typo into a destructive operation.
    if (new_quantity == 0) return SubmitResult{RejectReason::InvalidQuantity, 0, false, {}};

    auto loc_it = locations_.find(id);
    if (loc_it == locations_.end()) return SubmitResult{RejectReason::UnknownOrder, new_quantity, false, {}};

    const Location& loc = loc_it->second;

    // Shrinking at an unchanged price keeps queue position. This is safe to do
    // in place: the order is already resting, so by definition it wasn't
    // crossing, and taking size off it cannot make it start.
    if (new_price == loc.price && new_quantity <= loc.it->quantity) {
        loc.it->quantity = new_quantity;
        return SubmitResult{RejectReason::None, new_quantity, false, {}};
    }

    // Otherwise the order surrenders time priority. Copy what's needed out
    // first -- the cancel below invalidates `loc`. Cancelling rather than
    // splicing is what clears the locations_ entry, without which the
    // resubmission would be rejected as a duplicate id.
    const Side side = loc.side;
    const ParticipantId participant = loc.it->participant;
    cancelOrder(id);
    return addLimitOrder(id, side, new_price, new_quantity, participant);
}

std::vector<Order> OrderBook::restingOrders() const {
    std::vector<Order> orders;
    orders.reserve(locations_.size());
    // Both maps already iterate best-price-first, and each level's list is in
    // arrival order, so a straight walk produces exactly the required sequence.
    for (const auto& level : bids_) {
        for (const Order& order : level.second) orders.push_back(order);
    }
    for (const auto& level : asks_) {
        for (const Order& order : level.second) orders.push_back(order);
    }
    return orders;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

}  // namespace matching_engine
