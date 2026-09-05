#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>  // mkdtemp, rmdir -- POSIX, same as the rest of the project (server.cpp)
#include <vector>

#include "compaction.hpp"
#include "event_log.hpp"
#include "recovery.hpp"
#include "snapshot.hpp"
#include "wire.hpp"

#include "matching_engine.hpp"
#include "order_book.hpp"

using namespace matching_engine;

// Deliberately not CHECK(). CHECK() expands to nothing when NDEBUG is
// defined, and NDEBUG is exactly what a CMake Release build defines -- so an
// assert-based suite still prints "passed" for every test while verifying
// nothing at all. CHECK is always evaluated, in every build type.
namespace {

void checkFailed(const char* expression, const char* file, int line) {
    std::cout << "\nCHECK FAILED: " << expression << "\n  at " << file << ":" << line << "\n";
    std::exit(1);
}

}  // namespace

#define CHECK(condition)                                       \
    do {                                                       \
        if (!(condition)) checkFailed(#condition, __FILE__, __LINE__); \
    } while (false)

// Prices below are written with a digit separator so they read as
// dollars-and-cents: 100'00 is $100.00, i.e. 10000 ticks.

namespace {

void test_basic_match() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 10);
    auto trades = book.addLimitOrder(2, Side::Buy, 100'00, 10).trades;

    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 100'00);
    CHECK(trades[0].quantity == 10);
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
    std::cout << "test_basic_match passed\n";
}

void test_partial_fill_rests_remainder() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);
    auto trades = book.addLimitOrder(2, Side::Buy, 100'00, 10).trades;

    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 5);
    CHECK(book.bestBid().has_value() && *book.bestBid() == 100'00);
    CHECK(book.restingOrderCount() == 1);
    std::cout << "test_partial_fill_rests_remainder passed\n";
}

void test_price_priority_beats_time() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 101'00, 5);  // worse price, arrived first
    book.addLimitOrder(2, Side::Sell, 100'00, 5);  // better price, arrived second
    auto trades = book.addLimitOrder(3, Side::Buy, 101'00, 5).trades;

    CHECK(trades.size() == 1);
    CHECK(trades[0].sell_order_id == 2);  // best price fills first regardless of arrival
    std::cout << "test_price_priority_beats_time passed\n";
}

void test_time_priority_within_same_price() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);  // arrived first
    book.addLimitOrder(2, Side::Sell, 100'00, 5);  // arrived second, same price
    auto trades = book.addLimitOrder(3, Side::Buy, 100'00, 5).trades;

    CHECK(trades.size() == 1);
    CHECK(trades[0].sell_order_id == 1);  // first in, first filled
    std::cout << "test_time_priority_within_same_price passed\n";
}

void test_cancel_removes_order() {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 99'00, 5);
    CHECK(book.cancelOrder(1));
    CHECK(!book.cancelOrder(1));  // already gone
    CHECK(!book.bestBid().has_value());
    std::cout << "test_cancel_removes_order passed\n";
}

void test_market_order_sweeps_multiple_levels() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);
    book.addLimitOrder(2, Side::Sell, 101'00, 5);
    auto trades = book.addMarketOrder(3, Side::Buy, 8).trades;

    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100'00 && trades[0].quantity == 5);
    CHECK(trades[1].price == 101'00 && trades[1].quantity == 3);
    std::cout << "test_market_order_sweeps_multiple_levels passed\n";
}

void test_market_order_never_rests() {
    OrderBook book;
    // No resting sell orders at all -- market buy should just find nothing.
    auto trades = book.addMarketOrder(1, Side::Buy, 100).trades;
    CHECK(trades.empty());
    CHECK(book.restingOrderCount() == 0);  // unfilled market order is discarded, not resting
    std::cout << "test_market_order_never_rests passed\n";
}

void test_limit_order_does_not_cross_worse_price() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 101'00, 5);
    auto trades = book.addLimitOrder(2, Side::Buy, 100'00, 5).trades;  // won't pay 101

    CHECK(trades.empty());
    CHECK(book.bestBid().has_value() && *book.bestBid() == 100'00);
    CHECK(book.bestAsk().has_value() && *book.bestAsk() == 101'00);
    std::cout << "test_limit_order_does_not_cross_worse_price passed\n";
}

void test_duplicate_id_rejected_and_book_untouched() {
    OrderBook book;
    CHECK(book.addLimitOrder(1, Side::Buy, 100'00, 5).accepted());

    auto dup = book.addLimitOrder(1, Side::Buy, 101'00, 5);  // same id, still resting
    CHECK(!dup.accepted());
    CHECK(dup.trades.empty());

    // The book must look exactly as it did before the rejected submission:
    // one resting order at 100, and no phantom level at 101.
    CHECK(book.restingOrderCount() == 1);
    CHECK(book.bestBid().has_value() && *book.bestBid() == 100'00);

    // And the original is still reachable -- the whole point of rejecting.
    CHECK(book.cancelOrder(1));
    CHECK(book.restingOrderCount() == 0);
    CHECK(!book.bestBid().has_value());
    std::cout << "test_duplicate_id_rejected_and_book_untouched passed\n";
}

void test_duplicate_id_rejected_for_market_order() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);   // id 1 resting on the ask side
    book.addLimitOrder(2, Side::Sell, 100'00, 5);

    // A market buy reusing id 1 would otherwise trade against resting order 1
    // and erase its location entry while it is still in the book.
    auto dup = book.addMarketOrder(1, Side::Buy, 3);
    CHECK(!dup.accepted());
    CHECK(dup.trades.empty());
    CHECK(book.restingOrderCount() == 2);  // nothing consumed

    // A fresh id sweeps normally.
    auto ok = book.addMarketOrder(3, Side::Buy, 3);
    CHECK(ok.accepted());
    CHECK(ok.trades.size() == 1 && ok.trades[0].quantity == 3);
    std::cout << "test_duplicate_id_rejected_for_market_order passed\n";
}

void test_id_is_reusable_once_no_longer_resting() {
    OrderBook book;

    // Retired by cancellation.
    book.addLimitOrder(1, Side::Buy, 100'00, 5);
    CHECK(book.cancelOrder(1));
    CHECK(book.addLimitOrder(1, Side::Buy, 99'00, 5).accepted());
    CHECK(book.bestBid().has_value() && *book.bestBid() == 99'00);

    // Retired by being fully filled.
    book.addLimitOrder(2, Side::Sell, 99'00, 5);  // consumes order 1 completely
    CHECK(book.restingOrderCount() == 0);
    CHECK(book.addLimitOrder(1, Side::Buy, 98'00, 5).accepted());
    CHECK(book.restingOrderCount() == 1);
    std::cout << "test_id_is_reusable_once_no_longer_resting passed\n";
}

void test_fully_filled_incoming_order_does_not_reserve_its_id() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);

    // Order 2 crosses fully, so it never rests and never enters locations_.
    auto trades = book.addLimitOrder(2, Side::Buy, 100'00, 5).trades;
    CHECK(trades.size() == 1);
    CHECK(book.restingOrderCount() == 0);

    // Its id must therefore be immediately available again.
    CHECK(book.addLimitOrder(2, Side::Buy, 95'00, 5).accepted());
    std::cout << "test_fully_filled_incoming_order_does_not_reserve_its_id passed\n";
}

// A price reached by tick arithmetic must be identical to the same price
// written as a literal. Under `double`, stepping 100.00 up by 0.01 ten times
// produced 100.10000000000005 while the literal 100.10 was 100.09999999999999,
// so these two orders sat crossed in the book without ever matching.
void test_arithmetic_price_crosses_literal_price() {
    Price stepped = 100'00;
    for (int i = 0; i < 10; ++i) stepped += 1;  // ten ticks up
    CHECK(stepped == 100'10);                  // exact: no drift to accumulate

    OrderBook book;
    book.addLimitOrder(1, Side::Sell, stepped, 5);
    auto result = book.addLimitOrder(2, Side::Buy, 100'10, 5);

    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].price == 100'10);
    CHECK(book.restingOrderCount() == 0);  // no locked book left behind
    std::cout << "test_arithmetic_price_crosses_literal_price passed\n";
}

// The same equality failure used to split one economic price level into two
// map keys, so a sweep would fill only the first and strand the rest behind
// an invisible sub-level -- silently breaking time priority too.
void test_same_price_is_a_single_level() {
    Price stepped = 100'00;
    for (int i = 0; i < 10; ++i) stepped += 1;

    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'10, 5);   // arrived first
    book.addLimitOrder(2, Side::Sell, stepped, 5);  // same level, not a new one
    auto result = book.addLimitOrder(3, Side::Buy, 100'10, 10);

    CHECK(result.trades.size() == 2);              // both consumed by one sweep
    CHECK(result.trades[0].sell_order_id == 1);    // FIFO within the level
    CHECK(result.trades[1].sell_order_id == 2);
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_same_price_is_a_single_level passed\n";
}

void test_sub_unit_prices_are_exact() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'50, 10);
    auto result = book.addLimitOrder(2, Side::Buy, 100'50, 4);

    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].price == 100'50);
    CHECK(result.trades[0].quantity == 4);
    CHECK(book.bestAsk().has_value() && *book.bestAsk() == 100'50);
    std::cout << "test_sub_unit_prices_are_exact passed\n";
}

// ---- unfilled remainder ----

void test_unfilled_reports_rested_amount_for_limit_order() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 4);
    auto result = book.addLimitOrder(2, Side::Buy, 100'00, 10);

    CHECK(result.accepted());
    CHECK(result.unfilled == 6);  // 4 filled, 6 now resting
    CHECK(book.restingOrderCount() == 1);
    std::cout << "test_unfilled_reports_rested_amount_for_limit_order passed\n";
}

void test_unfilled_reports_discarded_amount_for_market_order() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 4);
    auto result = book.addMarketOrder(2, Side::Buy, 10);

    CHECK(result.accepted());
    CHECK(result.unfilled == 6);           // 4 filled, 6 discarded...
    CHECK(book.restingOrderCount() == 0);  // ...not rested
    std::cout << "test_unfilled_reports_discarded_amount_for_market_order passed\n";
}

void test_unfilled_is_full_quantity_on_rejection() {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 100'00, 5);
    auto dup = book.addLimitOrder(1, Side::Buy, 101'00, 7);

    CHECK(!dup.accepted());
    CHECK(dup.unfilled == 7);  // nothing filled, nothing rested
    std::cout << "test_unfilled_is_full_quantity_on_rejection passed\n";
}

// ---- cancel-replace ----

void test_modify_shrink_keeps_time_priority() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 10);  // arrived first
    book.addLimitOrder(2, Side::Sell, 100'00, 10);  // arrived second

    CHECK(book.modifyOrder(1, 100'00, 4).accepted());  // shrink, same price

    // Order 1 must still be at the front of the level.
    auto trades = book.addLimitOrder(3, Side::Buy, 100'00, 4).trades;
    CHECK(trades.size() == 1);
    CHECK(trades[0].sell_order_id == 1);
    std::cout << "test_modify_shrink_keeps_time_priority passed\n";
}

void test_modify_grow_surrenders_time_priority() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 10);  // arrived first
    book.addLimitOrder(2, Side::Sell, 100'00, 10);  // arrived second

    CHECK(book.modifyOrder(1, 100'00, 12).accepted());  // grow, same price

    // Order 1 went to the back, so order 2 is now first in line.
    auto trades = book.addLimitOrder(3, Side::Buy, 100'00, 4).trades;
    CHECK(trades.size() == 1);
    CHECK(trades[0].sell_order_id == 2);
    std::cout << "test_modify_grow_surrenders_time_priority passed\n";
}

void test_modify_reprice_surrenders_time_priority() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 10);
    book.addLimitOrder(2, Side::Sell, 101'00, 10);  // arrived second, worse price
    book.addLimitOrder(3, Side::Sell, 101'00, 10);  // arrived third, same level as 2

    // Move order 1 down to the 101 level; it should land behind 2 and 3 even
    // though it arrived before both.
    CHECK(book.modifyOrder(1, 101'00, 10).accepted());
    CHECK(!book.bestAsk().has_value() || *book.bestAsk() == 101'00);

    auto trades = book.addLimitOrder(4, Side::Buy, 101'00, 25).trades;
    CHECK(trades.size() == 3);
    CHECK(trades[0].sell_order_id == 2);
    CHECK(trades[1].sell_order_id == 3);
    CHECK(trades[2].sell_order_id == 1);  // repriced order is last
    std::cout << "test_modify_reprice_surrenders_time_priority passed\n";
}

void test_modify_into_a_crossing_price_trades() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 105'00, 10);  // resting well above the bid
    book.addLimitOrder(2, Side::Buy, 100'00, 4);

    // Reprice the ask down through the bid: it should trade on the way in,
    // at the resting bid's price, and rest the remainder.
    auto result = book.modifyOrder(1, 99'00, 10);
    CHECK(result.accepted());
    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].price == 100'00);  // resting order sets the price
    CHECK(result.trades[0].quantity == 4);
    CHECK(result.unfilled == 6);
    CHECK(book.bestAsk().has_value() && *book.bestAsk() == 99'00);
    std::cout << "test_modify_into_a_crossing_price_trades passed\n";
}

void test_modify_rejects_unknown_and_zero_quantity() {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 100'00, 5);

    CHECK(!book.modifyOrder(99, 100'00, 5).accepted());  // never existed
    CHECK(!book.modifyOrder(1, 100'00, 0).accepted());   // zero is not a cancel

    // Neither rejection may have disturbed the book.
    CHECK(book.restingOrderCount() == 1);
    CHECK(book.bestBid().has_value() && *book.bestBid() == 100'00);
    CHECK(book.cancelOrder(1));
    std::cout << "test_modify_rejects_unknown_and_zero_quantity passed\n";
}

// A repricing modify resubmits under the same id, so it has to clear the old
// locations_ entry first or addLimitOrder's duplicate-id check would reject
// the order out of its own book.
void test_modify_leaves_order_cancellable_and_accounted() {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 100'00, 5);
    CHECK(book.modifyOrder(1, 98'00, 7).accepted());
    CHECK(book.restingOrderCount() == 1);  // one order, not two
    CHECK(book.bestBid().has_value() && *book.bestBid() == 98'00);

    CHECK(book.cancelOrder(1));            // still reachable by id
    CHECK(book.restingOrderCount() == 0);
    CHECK(!book.bestBid().has_value());    // no orphan left at 100'00
    std::cout << "test_modify_leaves_order_cancellable_and_accounted passed\n";
}

void test_modify_a_fully_filled_order_is_rejected() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5);
    book.addLimitOrder(2, Side::Buy, 100'00, 5);  // consumes order 1 entirely
    CHECK(book.restingOrderCount() == 0);

    CHECK(!book.modifyOrder(1, 101'00, 5).accepted());  // nothing left to modify
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_modify_a_fully_filled_order_is_rejected passed\n";
}

// ---- self-trade prevention ----

constexpr ParticipantId kAlice = 1;
constexpr ParticipantId kBob = 2;

void test_self_trade_cancel_oldest_removes_resting_order() {
    OrderBook book;  // CancelOldest is the default
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kAlice);
    auto result = book.addLimitOrder(2, Side::Buy, 100'00, 5, kAlice);

    CHECK(result.accepted());
    CHECK(result.trades.empty());              // no wash trade
    CHECK(!result.cancelled_by_self_trade);    // the aggressor survived
    CHECK(result.unfilled == 5);

    // Alice's resting sell is gone and her buy took its place in the book.
    CHECK(!book.bestAsk().has_value());
    CHECK(book.bestBid().has_value() && *book.bestBid() == 100'00);
    CHECK(book.restingOrderCount() == 1);
    std::cout << "test_self_trade_cancel_oldest_removes_resting_order passed\n";
}

void test_self_trade_cancel_oldest_continues_into_next_order() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kAlice);  // front of the level
    book.addLimitOrder(2, Side::Sell, 100'00, 5, kBob);    // behind it

    auto result = book.addLimitOrder(3, Side::Buy, 100'00, 5, kAlice);

    // Alice's own order steps aside, and she trades with Bob's behind it.
    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].sell_order_id == 2);
    CHECK(result.unfilled == 0);
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_self_trade_cancel_oldest_continues_into_next_order passed\n";
}

void test_self_trade_cancel_newest_cancels_incoming_without_resting_it() {
    OrderBook book(SelfTradePolicy::CancelNewest);
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kAlice);
    auto result = book.addLimitOrder(2, Side::Buy, 100'00, 5, kAlice);

    CHECK(result.accepted());
    CHECK(result.trades.empty());
    CHECK(result.cancelled_by_self_trade);
    CHECK(result.unfilled == 5);  // the cancelled quantity, not a resting one

    // The incoming order must NOT be in the book -- resting it would leave
    // Alice crossed against herself.
    CHECK(book.restingOrderCount() == 1);
    CHECK(!book.bestBid().has_value());
    CHECK(book.bestAsk().has_value() && *book.bestAsk() == 100'00);
    std::cout << "test_self_trade_cancel_newest_cancels_incoming_without_resting_it passed\n";
}

void test_cancel_newest_keeps_fills_made_before_the_self_match() {
    OrderBook book(SelfTradePolicy::CancelNewest);
    book.addLimitOrder(1, Side::Sell, 100'00, 3, kBob);    // front: tradeable
    book.addLimitOrder(2, Side::Sell, 100'00, 5, kAlice);  // behind: Alice's own

    auto result = book.addLimitOrder(3, Side::Buy, 100'00, 10, kAlice);

    CHECK(result.trades.size() == 1);           // Bob's 3 filled first
    CHECK(result.trades[0].quantity == 3);
    CHECK(result.cancelled_by_self_trade);      // then it hit her own order
    CHECK(result.unfilled == 7);                // 10 - 3, cancelled not rested
    CHECK(book.restingOrderCount() == 1);       // only Alice's original sell
    std::cout << "test_cancel_newest_keeps_fills_made_before_the_self_match passed\n";
}

void test_different_participants_trade_normally() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kAlice);
    auto result = book.addLimitOrder(2, Side::Buy, 100'00, 5, kBob);

    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 5);
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_different_participants_trade_normally passed\n";
}

// The reserved anonymous id means "ownership not modelled", not "everyone is
// the same person" -- so it must never match itself as a self-trade.
void test_anonymous_orders_are_exempt_from_self_trade_prevention() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kNoParticipant);
    auto result = book.addLimitOrder(2, Side::Buy, 100'00, 5, kNoParticipant);

    CHECK(result.trades.size() == 1);      // they trade
    CHECK(!result.cancelled_by_self_trade);
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_anonymous_orders_are_exempt_from_self_trade_prevention passed\n";
}

void test_self_trade_prevention_applies_to_market_orders() {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 100'00, 5, kAlice);  // best ask, Alice's own
    book.addLimitOrder(2, Side::Sell, 101'00, 5, kBob);

    auto result = book.addMarketOrder(3, Side::Buy, 5, kAlice);

    // Alice's own order is cancelled out of the way, and the sweep continues
    // to the next level rather than stopping.
    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].sell_order_id == 2);
    CHECK(result.trades[0].price == 101'00);
    CHECK(book.restingOrderCount() == 0);
    std::cout << "test_self_trade_prevention_applies_to_market_orders passed\n";
}

void test_modify_inherits_the_participant() {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 100'00, 5, kAlice);
    CHECK(book.modifyOrder(1, 99'00, 8).accepted());  // repriced, so resubmitted

    // If the modify had dropped Alice's id, this would trade instead of
    // being prevented.
    auto result = book.addLimitOrder(2, Side::Sell, 99'00, 3, kAlice);
    CHECK(result.trades.empty());
    CHECK(book.bestBid() == std::nullopt);  // her repriced buy was cancelled
    std::cout << "test_modify_inherits_the_participant passed\n";
}

// ---- symbol router ----

void test_engine_rejects_orders_for_unregistered_symbols() {
    MatchingEngine engine;
    engine.addSymbol("AAPL");

    auto result = engine.addLimitOrder("MSFT", 1, Side::Buy, 100'00, 5);
    CHECK(!result.accepted());
    CHECK(result.reason == RejectReason::UnknownSymbol);
    CHECK(result.unfilled == 5);

    // A typo must not quietly open a new instrument.
    CHECK(engine.symbolCount() == 1);
    CHECK(!engine.hasSymbol("MSFT"));
    CHECK(engine.restingOrderCount() == 0);
    std::cout << "test_engine_rejects_orders_for_unregistered_symbols passed\n";
}

void test_engine_add_symbol_is_idempotent() {
    MatchingEngine engine;
    CHECK(engine.addSymbol("AAPL"));
    engine.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);

    CHECK(!engine.addSymbol("AAPL"));            // already registered
    CHECK(engine.restingOrderCount("AAPL") == 1);  // and its book survived
    std::cout << "test_engine_add_symbol_is_idempotent passed\n";
}

// The books are independent: same price, same order id, no interaction.
void test_engine_books_do_not_interact() {
    MatchingEngine engine;
    engine.addSymbol("AAPL");
    engine.addSymbol("MSFT");

    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5);
    auto result = engine.addLimitOrder("MSFT", 1, Side::Buy, 100'00, 5);

    // Reusing id 1 on another symbol is fine -- ids are scoped per book -- and
    // the crossing prices must not match across instruments.
    CHECK(result.accepted());
    CHECK(result.trades.empty());
    CHECK(engine.bestAsk("AAPL") == 100'00);
    CHECK(engine.bestBid("MSFT") == 100'00);
    CHECK(!engine.bestBid("AAPL").has_value());
    CHECK(!engine.bestAsk("MSFT").has_value());
    std::cout << "test_engine_books_do_not_interact passed\n";
}

void test_engine_routes_cancel_and_modify_by_symbol() {
    MatchingEngine engine;
    engine.addSymbol("AAPL");
    engine.addSymbol("MSFT");
    engine.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);

    CHECK(!engine.cancelOrder("MSFT", 1));   // right id, wrong book
    CHECK(!engine.cancelOrder("NVDA", 1));   // unknown symbol
    CHECK(engine.restingOrderCount() == 1);  // neither touched anything

    CHECK(!engine.modifyOrder("MSFT", 1, 99'00, 5).accepted());
    CHECK(engine.modifyOrder("AAPL", 1, 99'00, 5).accepted());
    CHECK(engine.bestBid("AAPL") == 99'00);

    CHECK(engine.cancelOrder("AAPL", 1));
    CHECK(engine.restingOrderCount() == 0);
    std::cout << "test_engine_routes_cancel_and_modify_by_symbol passed\n";
}

void test_engine_resting_count_aggregates_across_books() {
    MatchingEngine engine;
    engine.addSymbol("AAPL");
    engine.addSymbol("MSFT");
    engine.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);
    engine.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 5);
    engine.addLimitOrder("MSFT", 1, Side::Sell, 200'00, 5);

    CHECK(engine.restingOrderCount("AAPL") == 2);
    CHECK(engine.restingOrderCount("MSFT") == 1);
    CHECK(engine.restingOrderCount("NVDA") == 0);  // unknown symbol, not an error
    CHECK(engine.restingOrderCount() == 3);
    std::cout << "test_engine_resting_count_aggregates_across_books passed\n";
}

void test_engine_propagates_self_trade_policy_to_new_books() {
    MatchingEngine engine(SelfTradePolicy::CancelNewest);
    engine.addSymbol("AAPL");

    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5, kAlice);
    auto result = engine.addLimitOrder("AAPL", 2, Side::Buy, 100'00, 5, kAlice);

    CHECK(result.cancelled_by_self_trade);  // the book got the engine's policy
    CHECK(engine.restingOrderCount("AAPL") == 1);
    std::cout << "test_engine_propagates_self_trade_policy_to_new_books passed\n";
}

// ---- event log ----

void test_replay_reproduces_engine_state() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);

    live.addSymbol("AAPL");
    live.addSymbol("MSFT");
    live.addLimitOrder("AAPL", 1, Side::Sell, 100'50, 10, kAlice);
    live.addLimitOrder("AAPL", 2, Side::Buy, 100'50, 4, kBob);
    live.addLimitOrder("AAPL", 3, Side::Buy, 99'00, 7, kBob);
    live.modifyOrder("AAPL", 3, 99'50, 9);
    live.addMarketOrder("AAPL", 4, Side::Buy, 2, kBob);
    live.addLimitOrder("MSFT", 1, Side::Sell, 200'00, 5);
    live.cancelOrder("MSFT", 1);
    live.addLimitOrder("MSFT", 2, Side::Buy, 199'00, 3);

    MatchingEngine restored;
    std::istringstream in(log_stream.str());
    auto result = replay(in, restored);

    CHECK(!result.truncated);
    CHECK(result.applied == log.nextSequence());
    CHECK(live.restingOrderCount() > 0);  // the comparison isn't vacuous

    CHECK(restored.symbolCount() == live.symbolCount());
    CHECK(restored.restingOrderCount() == live.restingOrderCount());
    CHECK(restored.restingOrderCount("AAPL") == live.restingOrderCount("AAPL"));
    CHECK(restored.bestBid("AAPL") == live.bestBid("AAPL"));
    CHECK(restored.bestAsk("AAPL") == live.bestAsk("AAPL"));
    CHECK(restored.bestBid("MSFT") == live.bestBid("MSFT"));
    CHECK(restored.bestAsk("MSFT") == live.bestAsk("MSFT"));
    std::cout << "test_replay_reproduces_engine_state passed\n";
}

// The log is write-ahead, so it holds requests, not outcomes -- rejected ones
// included. Replaying them is harmless because they are rejected again.
void test_log_records_rejected_requests_and_replays_them_as_no_ops() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);

    live.addSymbol("AAPL");
    live.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);
    CHECK(!live.addLimitOrder("AAPL", 1, Side::Buy, 99'00, 5).accepted());   // duplicate id
    CHECK(!live.addLimitOrder("NVDA", 2, Side::Buy, 10'00, 5).accepted());   // unknown symbol

    CHECK(log.nextSequence() == 4);  // all four requests, not just the two that took

    MatchingEngine restored;
    std::istringstream in(log_stream.str());
    auto result = replay(in, restored);

    CHECK(result.applied == 4);
    CHECK(!result.truncated);
    CHECK(restored.restingOrderCount() == 1);
    CHECK(restored.bestBid("AAPL") == 100'00);
    CHECK(!restored.hasSymbol("NVDA"));
    std::cout << "test_log_records_rejected_requests_and_replays_them_as_no_ops passed\n";
}

// Restarting means replaying a log into an engine that is already logging to
// it. Without detaching, every replayed record would be written straight back
// out and the log would double on each restart.
void test_replay_does_not_append_to_an_attached_log() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);
    live.addSymbol("AAPL");
    live.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);

    const std::string captured = log_stream.str();
    const std::uint64_t records_before = log.nextSequence();

    MatchingEngine restarted;
    restarted.setEventLog(&log);
    std::istringstream in(captured);
    replay(in, restarted);

    CHECK(log.nextSequence() == records_before);  // nothing appended
    CHECK(log_stream.str() == captured);
    CHECK(restarted.eventLog() == &log);         // and logging is restored

    restarted.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 5);
    CHECK(log.nextSequence() == records_before + 1);  // still live afterwards
    std::cout << "test_replay_does_not_append_to_an_attached_log passed\n";
}

void test_replay_stops_at_a_torn_final_record() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);
    live.addSymbol("AAPL");
    live.addLimitOrder("AAPL", 1, Side::Buy, 100'00, 5);
    live.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 5);

    // A process killed mid-write leaves the last line half-written.
    std::string text = log_stream.str();
    const std::size_t last_newline = text.find_last_of('\n', text.size() - 2);
    text = text.substr(0, last_newline + 1) + "2 LIMIT AAPL 2 BUY";

    MatchingEngine restored;
    std::istringstream in(text);
    auto result = replay(in, restored);

    CHECK(result.truncated);
    CHECK(result.applied == 2);           // the two intact records still count
    CHECK(result.stopped_at_line == 3);
    CHECK(restored.restingOrderCount() == 1);
    CHECK(restored.bestBid("AAPL") == 100'00);
    std::cout << "test_replay_stops_at_a_torn_final_record passed\n";
}

void test_replay_stops_on_a_sequence_gap() {
    MatchingEngine restored;
    std::istringstream in("0 SYMBOL AAPL\n2 LIMIT AAPL 1 BUY 10000 5 0\n");  // seq 1 missing
    auto result = replay(in, restored);

    CHECK(result.truncated);
    CHECK(result.applied == 1);
    CHECK(result.stopped_at_line == 2);
    CHECK(restored.restingOrderCount() == 0);
    std::cout << "test_replay_stops_on_a_sequence_gap passed\n";
}

void test_symbols_must_be_a_single_token() {
    MatchingEngine engine;
    CHECK(!engine.addSymbol(""));       // would produce an unparseable record
    CHECK(!engine.addSymbol("A B"));
    CHECK(!engine.addSymbol("A\tB"));
    CHECK(engine.addSymbol("AAPL"));
    CHECK(engine.symbolCount() == 1);
    std::cout << "test_symbols_must_be_a_single_token passed\n";
}

void test_replay_preserves_participants() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);
    live.addSymbol("AAPL");
    live.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5, kAlice);

    MatchingEngine restored;
    std::istringstream in(log_stream.str());
    replay(in, restored);

    // Had the participant not survived the round trip, this would trade.
    auto result = restored.addLimitOrder("AAPL", 2, Side::Buy, 100'00, 5, kAlice);
    CHECK(result.trades.empty());
    CHECK(restored.restingOrderCount() == 1);
    std::cout << "test_replay_preserves_participants passed\n";
}

// ---- snapshots and compaction ----

namespace {

// Builds a representative engine: two instruments, resting depth on both sides,
// several orders queued at one price so time priority is actually observable,
// a modify, a partial fill, and an instrument left deliberately empty.
void buildRepresentativeState(MatchingEngine& engine) {
    engine.addSymbol("AAPL");
    engine.addSymbol("MSFT");
    engine.addSymbol("EMPTY");
    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'50, 10, kAlice);
    engine.addLimitOrder("AAPL", 2, Side::Sell, 100'50, 7, kBob);    // same level, behind 1
    engine.addLimitOrder("AAPL", 3, Side::Sell, 101'00, 4, kAlice);
    engine.addLimitOrder("AAPL", 4, Side::Buy, 99'00, 6, kBob);
    engine.addLimitOrder("AAPL", 5, Side::Buy, 99'00, 3, kAlice);    // same level, behind 4
    engine.modifyOrder("AAPL", 4, 99'50, 8);                         // repriced: goes to a new level
    engine.addLimitOrder("MSFT", 1, Side::Buy, 200'00, 5);
    engine.addMarketOrder("MSFT", 2, Side::Sell, 2);                 // partial fill of MSFT 1
}

void assertSameObservableState(const MatchingEngine& a, const MatchingEngine& b) {
    CHECK(a.symbolCount() == b.symbolCount());
    CHECK(a.symbols() == b.symbols());
    CHECK(a.restingOrderCount() == b.restingOrderCount());
    for (const Symbol& symbol : a.symbols()) {
        CHECK(a.restingOrderCount(symbol) == b.restingOrderCount(symbol));
        CHECK(a.bestBid(symbol) == b.bestBid(symbol));
        CHECK(a.bestAsk(symbol) == b.bestAsk(symbol));
        const OrderBook* book_a = a.book(symbol);
        const OrderBook* book_b = b.book(symbol);
        CHECK(book_a != nullptr && book_b != nullptr);
        const std::vector<Order> orders_a = book_a->restingOrders();
        const std::vector<Order> orders_b = book_b->restingOrders();
        CHECK(orders_a.size() == orders_b.size());
        // Compared in book order, so this checks queue position too, not just
        // which orders survived.
        for (std::size_t i = 0; i < orders_a.size(); ++i) {
            CHECK(orders_a[i].id == orders_b[i].id);
            CHECK(orders_a[i].participant == orders_b[i].participant);
            CHECK(orders_a[i].side == orders_b[i].side);
            CHECK(orders_a[i].price == orders_b[i].price);
            CHECK(orders_a[i].quantity == orders_b[i].quantity);
        }
    }
}

}  // namespace

void test_snapshot_round_trip_preserves_state_and_queue_order() {
    MatchingEngine live;
    buildRepresentativeState(live);
    CHECK(live.restingOrderCount() > 3);  // not a vacuous comparison

    std::ostringstream snap;
    writeSnapshot(snap, live, 42);

    MatchingEngine restored;
    std::istringstream in(snap.str());
    auto result = loadSnapshot(in, restored);

    CHECK(result.ok);
    CHECK(result.next_seq == 42);
    CHECK(result.orders_loaded == live.restingOrderCount());
    assertSameObservableState(live, restored);

    // An instrument with no orders is still an instrument.
    CHECK(restored.hasSymbol("EMPTY"));
    std::cout << "test_snapshot_round_trip_preserves_state_and_queue_order passed\n";
}

void test_snapshot_is_deterministic_for_identical_state() {
    MatchingEngine a;
    MatchingEngine b;
    buildRepresentativeState(a);
    buildRepresentativeState(b);

    std::ostringstream snap_a;
    std::ostringstream snap_b;
    writeSnapshot(snap_a, a, 7);
    writeSnapshot(snap_b, b, 7);

    // Byte-identical, which is what sorting the symbols buys.
    CHECK(snap_a.str() == snap_b.str());
    std::cout << "test_snapshot_is_deterministic_for_identical_state passed\n";
}

void test_truncated_snapshot_is_rejected() {
    MatchingEngine live;
    buildRepresentativeState(live);
    std::ostringstream snap;
    writeSnapshot(snap, live, 10);

    // Chop the END marker off, as a crash mid-write would.
    std::string text = snap.str();
    text = text.substr(0, text.rfind("END\n"));

    MatchingEngine restored;
    std::istringstream in(text);
    auto result = loadSnapshot(in, restored);

    CHECK(!result.ok);  // missing END, so the caller must discard `restored`
    std::cout << "test_truncated_snapshot_is_rejected passed\n";
}

void test_snapshot_of_unknown_version_is_rejected() {
    MatchingEngine restored;
    std::istringstream in("SNAPSHOT 99 5\nSYMBOL AAPL\nEND\n");
    auto result = loadSnapshot(in, restored);
    CHECK(!result.ok);
    std::cout << "test_snapshot_of_unknown_version_is_rejected passed\n";
}

// The whole point of compaction: snapshot at N, keep only the log from N on.
void test_compaction_recovers_snapshot_plus_log_tail() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);
    buildRepresentativeState(live);

    // Compact here.
    const std::uint64_t compact_at = log.nextSequence();
    std::ostringstream snap;
    writeSnapshot(snap, live, compact_at);

    // The log is truncated, and new activity continues the sequence.
    std::ostringstream tail_stream;
    EventLog tail(tail_stream, compact_at);
    live.setEventLog(&tail);
    live.addLimitOrder("AAPL", 6, Side::Buy, 98'00, 5, kBob);
    live.cancelOrder("AAPL", 3);
    live.addLimitOrder("MSFT", 3, Side::Sell, 205'00, 9, kAlice);

    MatchingEngine restored;
    std::istringstream snap_in(snap.str());
    auto snap_result = loadSnapshot(snap_in, restored);
    CHECK(snap_result.ok);

    std::istringstream tail_in(tail_stream.str());
    auto replay_result = replay(tail_in, restored, snap_result.next_seq);
    CHECK(!replay_result.truncated);
    CHECK(replay_result.applied == 3);

    assertSameObservableState(live, restored);
    std::cout << "test_compaction_recovers_snapshot_plus_log_tail passed\n";
}

// Compaction writes the snapshot before truncating the log. A crash in that
// window leaves a snapshot at N next to a log that still starts at 0, so
// replay must skip the already-snapshotted prefix rather than call it a gap.
void test_recovery_survives_a_crash_between_snapshot_and_truncation() {
    std::ostringstream log_stream;
    EventLog log(log_stream);
    MatchingEngine live;
    live.setEventLog(&log);
    buildRepresentativeState(live);

    const std::uint64_t compact_at = log.nextSequence();
    std::ostringstream snap;
    writeSnapshot(snap, live, compact_at);
    // Crash here: the snapshot exists, but the log was never truncated. More
    // requests then arrive and append to that same un-truncated log.
    live.addLimitOrder("AAPL", 6, Side::Buy, 98'00, 5, kBob);
    live.cancelOrder("AAPL", 3);

    MatchingEngine restored;
    std::istringstream snap_in(snap.str());
    auto snap_result = loadSnapshot(snap_in, restored);
    CHECK(snap_result.ok);

    std::istringstream log_in(log_stream.str());  // full log, from sequence 0
    auto replay_result = replay(log_in, restored, snap_result.next_seq);

    CHECK(!replay_result.truncated);   // the old prefix is skipped, not an error
    CHECK(replay_result.applied == 2);  // only the two records after the snapshot
    assertSameObservableState(live, restored);
    std::cout << "test_recovery_survives_a_crash_between_snapshot_and_truncation passed\n";
}

// ---- wire protocol ----

namespace {

std::vector<std::uint8_t> encodeAll(const std::vector<Request>& requests) {
    std::vector<std::uint8_t> bytes;
    for (const Request& request : requests) {
        const bool ok = encodeRequest(request, bytes);
        CHECK(ok);
    }
    return bytes;
}

Request limitRequest(std::uint32_t correlation, OrderId id, Side side, Price price, Quantity qty,
                     ParticipantId participant) {
    Request request;
    request.type = MessageType::LimitOrder;
    request.correlation_id = correlation;
    request.symbol = "AAPL";
    request.order_id = id;
    request.side = side;
    request.price = price;
    request.quantity = qty;
    request.participant = participant;
    return request;
}

// Feeds `bytes` through a FrameReader in two chunks split at `split`, and
// returns the decoded requests.
std::vector<Request> readSplitAt(const std::vector<std::uint8_t>& bytes, std::size_t split) {
    FrameReader reader;
    std::vector<Request> decoded;
    MessageType type;
    std::uint32_t correlation = 0;
    std::vector<std::uint8_t> payload;

    reader.append(bytes.data(), split);
    while (reader.next(type, correlation, payload)) {
        Request request;
        CHECK(decodeRequest(type, correlation, payload.data(), payload.size(), request));
        decoded.push_back(request);
    }
    reader.append(bytes.data() + split, bytes.size() - split);
    while (reader.next(type, correlation, payload)) {
        Request request;
        CHECK(decodeRequest(type, correlation, payload.data(), payload.size(), request));
        decoded.push_back(request);
    }
    CHECK(!reader.failed());
    return decoded;
}

}  // namespace

void test_wire_round_trips_every_request_type() {
    Request add;
    add.type = MessageType::AddSymbol;
    add.correlation_id = 1;
    add.symbol = "MSFT";

    Request market;
    market.type = MessageType::MarketOrder;
    market.correlation_id = 3;
    market.symbol = "AAPL";
    market.order_id = 77;
    market.side = Side::Sell;
    market.quantity = 12;
    market.participant = 5;

    Request modify;
    modify.type = MessageType::ModifyOrder;
    modify.correlation_id = 4;
    modify.symbol = "AAPL";
    modify.order_id = 77;
    modify.price = 98'25;
    modify.quantity = 3;

    Request cancel;
    cancel.type = MessageType::CancelOrder;
    cancel.correlation_id = 5;
    cancel.symbol = "AAPL";
    cancel.order_id = 77;

    // A negative price is not a valid order, but the field is signed and the
    // encoding of a negative i64 is exactly where a hand-rolled codec breaks.
    const std::vector<Request> sent = {add, limitRequest(2, 42, Side::Buy, 100'50, 9, 7), market,
                                       modify, cancel, limitRequest(6, 1, Side::Buy, -12'34, 1, 0)};
    const std::vector<Request> got = readSplitAt(encodeAll(sent), 0);

    CHECK(got.size() == sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        CHECK(got[i].type == sent[i].type);
        CHECK(got[i].correlation_id == sent[i].correlation_id);
        CHECK(got[i].symbol == sent[i].symbol);
        CHECK(got[i].order_id == sent[i].order_id);
        CHECK(got[i].price == sent[i].price);
        CHECK(got[i].quantity == sent[i].quantity);
        CHECK(got[i].participant == sent[i].participant);
    }
    std::cout << "test_wire_round_trips_every_request_type passed\n";
}

// TCP splits wherever it likes. Rather than sample a few boundaries, split at
// every single one -- including mid-header and mid-payload -- and require the
// decoded result to be identical each time.
void test_framing_is_identical_at_every_possible_split() {
    const std::vector<Request> sent = {limitRequest(1, 10, Side::Buy, 100'00, 5, 7),
                                       limitRequest(2, 11, Side::Sell, 101'00, 6, 9),
                                       limitRequest(3, 12, Side::Buy, 99'00, 7, 0)};
    const std::vector<std::uint8_t> bytes = encodeAll(sent);
    CHECK(bytes.size() > kHeaderBytes * 3);

    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        const std::vector<Request> got = readSplitAt(bytes, split);
        CHECK(got.size() == sent.size());
        for (std::size_t i = 0; i < sent.size(); ++i) {
            CHECK(got[i].correlation_id == sent[i].correlation_id);
            CHECK(got[i].order_id == sent[i].order_id);
            CHECK(got[i].price == sent[i].price);
            CHECK(got[i].quantity == sent[i].quantity);
        }
    }
    std::cout << "test_framing_is_identical_at_every_possible_split passed\n";
}

void test_framing_yields_nothing_until_a_frame_is_complete() {
    std::vector<std::uint8_t> bytes;
    CHECK(encodeRequest(limitRequest(1, 10, Side::Buy, 100'00, 5, 7), bytes));

    FrameReader reader;
    MessageType type;
    std::uint32_t correlation = 0;
    std::vector<std::uint8_t> payload;

    // One byte short: still nothing, and still not a failure.
    reader.append(bytes.data(), bytes.size() - 1);
    CHECK(!reader.next(type, correlation, payload));
    CHECK(!reader.failed());

    reader.append(bytes.data() + bytes.size() - 1, 1);
    CHECK(reader.next(type, correlation, payload));
    CHECK(!reader.next(type, correlation, payload));  // and only the one
    std::cout << "test_framing_yields_nothing_until_a_frame_is_complete passed\n";
}

void test_framing_rejects_bad_version_and_oversized_payloads() {
    std::vector<std::uint8_t> bytes;
    CHECK(encodeRequest(limitRequest(1, 10, Side::Buy, 100'00, 5, 7), bytes));

    {
        std::vector<std::uint8_t> corrupt = bytes;
        corrupt[9] = kWireVersion + 1;  // version byte
        FrameReader reader;
        MessageType type;
        std::uint32_t correlation = 0;
        std::vector<std::uint8_t> payload;
        reader.append(corrupt.data(), corrupt.size());
        CHECK(!reader.next(type, correlation, payload));
        CHECK(reader.failed());
    }
    {
        // A client claiming a huge payload must not make the server buffer it.
        std::vector<std::uint8_t> corrupt = bytes;
        corrupt[0] = 0xFF;
        corrupt[1] = 0xFF;
        corrupt[2] = 0xFF;
        corrupt[3] = 0xFF;
        FrameReader reader;
        MessageType type;
        std::uint32_t correlation = 0;
        std::vector<std::uint8_t> payload;
        reader.append(corrupt.data(), corrupt.size());
        CHECK(!reader.next(type, correlation, payload));
        CHECK(reader.failed());
    }
    std::cout << "test_framing_rejects_bad_version_and_oversized_payloads passed\n";
}

void test_decode_rejects_wrong_payload_length() {
    std::vector<std::uint8_t> payload(4, 0);  // far too short for a limit order
    Request request;
    CHECK(!decodeRequest(MessageType::LimitOrder, 1, payload.data(), payload.size(), request));

    // Trailing bytes are rejected too: they mean the ends disagree on format.
    std::vector<std::uint8_t> bytes;
    CHECK(encodeRequest(limitRequest(1, 10, Side::Buy, 100'00, 5, 7), bytes));
    const std::size_t body = bytes.size() - kHeaderBytes;
    CHECK(!decodeRequest(MessageType::LimitOrder, 1, bytes.data() + kHeaderBytes, body + 1, request));
    std::cout << "test_decode_rejects_wrong_payload_length passed\n";
}

void test_encode_rejects_an_over_long_symbol() {
    Request request = limitRequest(1, 10, Side::Buy, 100'00, 5, 7);
    request.symbol = "TOOLONGSYMBOL";  // truncating would alias two instruments
    std::vector<std::uint8_t> bytes;
    CHECK(!encodeRequest(request, bytes));
    CHECK(bytes.empty());  // and nothing half-written is left behind
    std::cout << "test_encode_rejects_an_over_long_symbol passed\n";
}

void test_response_round_trips_with_trades() {
    MatchingEngine engine;
    engine.addSymbol("AAPL");
    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5, kAlice);
    engine.addLimitOrder("AAPL", 2, Side::Sell, 101'00, 5, kAlice);

    Request sweep;
    sweep.type = MessageType::MarketOrder;
    sweep.correlation_id = 99;
    sweep.symbol = "AAPL";
    sweep.order_id = 3;
    sweep.side = Side::Buy;
    sweep.quantity = 12;
    sweep.participant = kBob;

    const Response sent = applyRequest(sweep, engine);
    CHECK(sent.trades.size() == 2);
    CHECK(sent.unfilled == 2);

    std::vector<std::uint8_t> bytes;
    encodeResponse(sent, bytes);

    FrameReader reader;
    MessageType type;
    std::uint32_t correlation = 0;
    std::vector<std::uint8_t> payload;
    reader.append(bytes.data(), bytes.size());
    CHECK(reader.next(type, correlation, payload));
    CHECK(type == MessageType::Response);

    Response got;
    CHECK(decodeResponse(correlation, payload.data(), payload.size(), got));
    CHECK(got.correlation_id == 99);
    CHECK(got.reason == RejectReason::None);
    CHECK(got.unfilled == sent.unfilled);
    CHECK(got.trades.size() == sent.trades.size());
    for (std::size_t i = 0; i < sent.trades.size(); ++i) {
        CHECK(got.trades[i].buy_order_id == sent.trades[i].buy_order_id);
        CHECK(got.trades[i].sell_order_id == sent.trades[i].sell_order_id);
        CHECK(got.trades[i].price == sent.trades[i].price);
        CHECK(got.trades[i].quantity == sent.trades[i].quantity);
    }
    std::cout << "test_response_round_trips_with_trades passed\n";
}

void test_apply_request_reports_rejections_over_the_wire() {
    MatchingEngine engine;

    Request unknown = limitRequest(1, 10, Side::Buy, 100'00, 5, 7);  // AAPL not registered
    Response response = applyRequest(unknown, engine);
    CHECK(response.reason == RejectReason::UnknownSymbol);
    CHECK(response.unfilled == 5);

    Request add;
    add.type = MessageType::AddSymbol;
    add.symbol = "AAPL";
    CHECK(applyRequest(add, engine).reason == RejectReason::None);
    CHECK(applyRequest(add, engine).reason != RejectReason::None);  // already registered

    CHECK(applyRequest(limitRequest(2, 10, Side::Buy, 100'00, 5, 7), engine).reason ==
           RejectReason::None);
    CHECK(applyRequest(limitRequest(3, 10, Side::Buy, 99'00, 5, 7), engine).reason ==
           RejectReason::DuplicateOrderId);
    std::cout << "test_apply_request_reports_rejections_over_the_wire passed\n";
}

// ---- authentication (wire.hpp's MessageType::Authenticate) ----

void test_authenticate_round_trips_the_raw_token() {
    // A char array, not assigned through operator=(const char*): that
    // assignment goes through strlen() and would truncate at the embedded
    // nul below before this test ever got to exercise anything. sizeof - 1
    // keeps every byte the literal actually wrote, embedded nul included,
    // while dropping only the compiler's own trailing terminator.
    static const char kRawToken[] = "a token with spaces and \0 an embedded nul";

    Request request;
    request.type = MessageType::Authenticate;
    request.correlation_id = 42;
    request.token.assign(kRawToken, sizeof(kRawToken) - 1);
    // Exactly what a length-prefixed payload survives and a length implied
    // by a nul terminator (like the symbol field) would not -- the reason
    // Authenticate carries no such field.
    CHECK(request.token.find('\0') != std::string::npos);

    std::vector<std::uint8_t> bytes;
    CHECK(encodeRequest(request, bytes));

    FrameReader reader;
    reader.append(bytes.data(), bytes.size());
    MessageType type;
    std::uint32_t correlation_id = 0;
    std::vector<std::uint8_t> payload;
    CHECK(reader.next(type, correlation_id, payload));
    CHECK(type == MessageType::Authenticate);

    Request decoded;
    CHECK(decodeRequest(type, correlation_id, payload.data(), payload.size(), decoded));
    CHECK(decoded.correlation_id == 42);
    CHECK(decoded.token == request.token);
    std::cout << "test_authenticate_round_trips_the_raw_token passed\n";
}

void test_authenticate_with_an_empty_token_round_trips() {
    Request request;
    request.type = MessageType::Authenticate;
    request.correlation_id = 1;
    // request.token left as its default-constructed "" -- an empty frame is
    // the case most likely to trip a null-pointer edge case in decoding.

    std::vector<std::uint8_t> bytes;
    CHECK(encodeRequest(request, bytes));
    CHECK(bytes.size() == kHeaderBytes);  // header only, zero-byte payload

    FrameReader reader;
    reader.append(bytes.data(), bytes.size());
    MessageType type;
    std::uint32_t correlation_id = 0;
    std::vector<std::uint8_t> payload;
    CHECK(reader.next(type, correlation_id, payload));
    CHECK(payload.empty());

    Request decoded;
    CHECK(decodeRequest(type, correlation_id, payload.data(), payload.size(), decoded));
    CHECK(decoded.token.empty());
    std::cout << "test_authenticate_with_an_empty_token_round_trips passed\n";
}

// applyRequest never actually sees a live Authenticate request -- OrderServer
// intercepts it first, since the engine has no notion of connections or
// credentials -- but its switch still has to handle the type, so this pins
// down what that unreachable-in-practice branch does.
void test_apply_request_does_not_understand_authenticate() {
    MatchingEngine engine;
    Request request;
    request.type = MessageType::Authenticate;
    request.token = "irrelevant-here";
    CHECK(applyRequest(request, engine).reason != RejectReason::None);
    std::cout << "test_apply_request_does_not_understand_authenticate passed\n";
}

void test_reject_reason_describe_covers_auth_reasons() {
    CHECK(std::string(describe(RejectReason::NotAuthenticated)) == "not authenticated");
    CHECK(std::string(describe(RejectReason::AuthenticationFailed)) == "authentication failed");
    std::cout << "test_reject_reason_describe_covers_auth_reasons passed\n";
}

// ---- shared recovery (recovery.hpp) ----
//
// The CLI and the server used to each carry their own ~50-line copy of
// "load the snapshot, replay the log from its sequence, refuse to proceed
// past a torn log, open the log for continued append". These exercise the
// single shared implementation directly, on real files, rather than only
// indirectly through the two mains.

namespace {

std::string makeTempDir() {
    std::string tmpl = "/tmp/matching_engine_recovery_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = ::mkdtemp(buf.data());
    CHECK(result != nullptr);
    return std::string(result);
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void removeIfExists(const std::string& path) { std::remove(path.c_str()); }

}  // namespace

void test_recover_from_nonexistent_log_starts_empty_and_logs_from_zero() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";

    MatchingEngine engine;
    RecoveredLog recovered = recoverAndOpenLog(log_path, engine);

    CHECK(recovered.ok);
    CHECK(!recovered.snapshot_present);
    CHECK(!recovered.log_present);  // nothing existed to replay
    CHECK(recovered.records_replayed == 0);
    CHECK(engine.restingOrderCount() == 0);
    CHECK(engine.eventLog() == recovered.log.get());  // attached, ready to log

    // The log is nonetheless open for writing, starting at sequence 0.
    engine.addSymbol("AAPL");
    CHECK(readFile(log_path) == "0 SYMBOL AAPL\n");

    removeIfExists(log_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_recover_from_nonexistent_log_starts_empty_and_logs_from_zero passed\n";
}

void test_recover_from_existing_log_replays_and_continues_sequence() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";

    // Build the log by actually running the engine, the same way the real
    // log got there, rather than hand-writing a record format that could
    // drift from what EventLog actually produces.
    {
        MatchingEngine seed;
        RecoveredLog first = recoverAndOpenLog(log_path, seed);
        CHECK(first.ok);
        seed.addSymbol("AAPL");
        seed.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5);
        // first.file/first.log go out of scope here, flushing and closing.
    }

    MatchingEngine restored;
    RecoveredLog recovered = recoverAndOpenLog(log_path, restored);

    CHECK(recovered.ok);
    CHECK(!recovered.snapshot_present);
    CHECK(recovered.log_present);
    CHECK(recovered.records_replayed == 2);  // SYMBOL + LIMIT
    CHECK(restored.restingOrderCount() == 1);
    CHECK(restored.bestAsk("AAPL") == 100'00);

    // Appending must continue the sequence rather than restart it at 0,
    // which would collide with the two records already in the file.
    restored.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 3);
    const std::string content = readFile(log_path);
    CHECK(content == "0 SYMBOL AAPL\n1 LIMIT AAPL 1 SELL 10000 5 0\n2 LIMIT AAPL 2 BUY 9900 3 0\n");

    removeIfExists(log_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_recover_from_existing_log_replays_and_continues_sequence passed\n";
}

void test_recover_from_snapshot_and_log_tail_on_disk() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";
    const std::string snapshot_path = log_path + ".snapshot";

    {
        MatchingEngine seed;
        RecoveredLog first = recoverAndOpenLog(log_path, seed);
        CHECK(first.ok);
        seed.addSymbol("AAPL");
        seed.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5);

        // Compact by hand: snapshot at the current sequence, then start a
        // fresh log file continuing from there -- mirroring what the CLI's
        // COMPACT command does.
        const std::uint64_t at = first.log->nextSequence();
        std::ofstream snap_out(snapshot_path, std::ios::trunc);
        writeSnapshot(snap_out, seed, at);
        snap_out.close();
        first.file->close();
        std::ofstream(log_path, std::ios::trunc).close();  // truncate
    }

    // A record added after compaction, appended directly (as a real process
    // restarting after COMPACT would do via recoverAndOpenLog).
    {
        MatchingEngine seed2;
        RecoveredLog reopened = recoverAndOpenLog(log_path, seed2);
        CHECK(reopened.ok);
        CHECK(reopened.snapshot_present);
        seed2.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 3);
    }

    MatchingEngine restored;
    RecoveredLog recovered = recoverAndOpenLog(log_path, restored);

    CHECK(recovered.ok);
    CHECK(recovered.snapshot_present);
    CHECK(recovered.snapshot_orders_loaded == 1);
    CHECK(recovered.log_present);
    CHECK(recovered.records_replayed == 1);  // just the post-compaction LIMIT
    CHECK(restored.restingOrderCount() == 2);
    CHECK(restored.bestAsk("AAPL") == 100'00);
    CHECK(restored.bestBid("AAPL") == 99'00);

    removeIfExists(log_path);
    removeIfExists(snapshot_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_recover_from_snapshot_and_log_tail_on_disk passed\n";
}

void test_recover_refuses_a_truncated_log_on_disk() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";

    writeFile(log_path, "0 SYMBOL AAPL\n1 LIMIT AAPL 1 SELL");  // torn mid-record

    MatchingEngine engine;
    RecoveredLog recovered = recoverAndOpenLog(log_path, engine);

    CHECK(!recovered.ok);
    CHECK(recovered.error.find("damaged") != std::string::npos);
    CHECK(recovered.file == nullptr);  // never opened for append
    CHECK(recovered.log == nullptr);

    removeIfExists(log_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_recover_refuses_a_truncated_log_on_disk passed\n";
}

void test_recover_refuses_an_incomplete_snapshot_on_disk() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";
    const std::string snapshot_path = log_path + ".snapshot";

    // A snapshot with no END marker, as a crash mid-write would leave.
    writeFile(snapshot_path, "SNAPSHOT 1 5\nSYMBOL AAPL\n");

    MatchingEngine engine;
    RecoveredLog recovered = recoverAndOpenLog(log_path, engine);

    CHECK(!recovered.ok);
    CHECK(recovered.error.find("snapshot") != std::string::npos);
    CHECK(recovered.file == nullptr);
    CHECK(recovered.log == nullptr);

    removeIfExists(snapshot_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_recover_refuses_an_incomplete_snapshot_on_disk passed\n";
}

// ---- compaction (compaction.hpp) ----

void test_compact_writes_snapshot_and_truncates_log() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";
    const std::string snapshot_path = log_path + ".snapshot";

    MatchingEngine engine;
    RecoveredLog recovered = recoverAndOpenLog(log_path, engine);
    CHECK(recovered.ok);
    engine.addSymbol("AAPL");
    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5);

    const CompactionResult result =
        compact(engine, snapshot_path, *recovered.log, *recovered.file, log_path);

    CHECK(result.ok);
    CHECK(result.sequence == 2);  // SYMBOL + LIMIT
    CHECK(readFile(log_path).empty());  // truncated
    CHECK(readFile(snapshot_path).find("ORDER AAPL 1 SELL 10000 5 0") != std::string::npos);

    // The log keeps working afterward, continuing the same sequence -- it is
    // the same EventLog object, reused rather than replaced.
    engine.addLimitOrder("AAPL", 2, Side::Buy, 99'00, 3);
    CHECK(readFile(log_path) == "2 LIMIT AAPL 2 BUY 9900 3 0\n");

    removeIfExists(log_path);
    removeIfExists(snapshot_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_compact_writes_snapshot_and_truncates_log passed\n";
}

void test_auto_compactor_triggers_on_record_count() {
    AutoCompactor compactor(CompactionPolicy{/*max_records=*/3, std::nullopt});

    CHECK(!compactor.shouldCompact(0));
    CHECK(!compactor.shouldCompact(2));
    CHECK(compactor.shouldCompact(3));
    CHECK(compactor.shouldCompact(10));
    std::cout << "test_auto_compactor_triggers_on_record_count passed\n";
}

void test_auto_compactor_triggers_on_age() {
    AutoCompactor compactor(
        CompactionPolicy{std::nullopt, std::chrono::milliseconds(20)});

    CHECK(!compactor.shouldCompact(0));  // not enough time has passed yet
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(compactor.shouldCompact(0));  // sequence unchanged; age alone trips it
    std::cout << "test_auto_compactor_triggers_on_age passed\n";
}

void test_auto_compactor_maybe_compact_resets_after_success() {
    const std::string dir = makeTempDir();
    const std::string log_path = dir + "/book.log";
    const std::string snapshot_path = log_path + ".snapshot";

    MatchingEngine engine;
    RecoveredLog recovered = recoverAndOpenLog(log_path, engine);
    CHECK(recovered.ok);
    engine.addSymbol("AAPL");  // sequence 0: 1 record so far

    AutoCompactor compactor(CompactionPolicy{/*max_records=*/2, std::nullopt});

    // Below threshold: nothing happens, and the log is untouched.
    auto attempt = compactor.maybeCompact(engine, snapshot_path, *recovered.log, *recovered.file,
                                          log_path);
    CHECK(!attempt.has_value());
    CHECK(readFile(log_path) == "0 SYMBOL AAPL\n");

    engine.addLimitOrder("AAPL", 1, Side::Sell, 100'00, 5);  // sequence 1: now 2 records since

    attempt = compactor.maybeCompact(engine, snapshot_path, *recovered.log, *recovered.file, log_path);
    CHECK(attempt.has_value());
    CHECK(attempt->ok);
    CHECK(readFile(log_path).empty());

    // Immediately after a successful compaction, the counter has reset: with
    // nothing new logged, it must not fire again.
    attempt = compactor.maybeCompact(engine, snapshot_path, *recovered.log, *recovered.file, log_path);
    CHECK(!attempt.has_value());

    removeIfExists(log_path);
    removeIfExists(snapshot_path);
    CHECK(::rmdir(dir.c_str()) == 0);
    std::cout << "test_auto_compactor_maybe_compact_resets_after_success passed\n";
}

void test_parse_compaction_policy_args() {
    {
        const char* argv[] = {"prog", "book.log"};
        auto result = parseCompactionPolicyArgs(2, const_cast<char**>(argv), 2);
        CHECK(result.ok);
        CHECK(!result.policy.has_value());  // no flags given
    }
    {
        const char* argv[] = {"prog", "book.log", "--auto-compact-records=500",
                              "--auto-compact-seconds=60"};
        auto result = parseCompactionPolicyArgs(4, const_cast<char**>(argv), 2);
        CHECK(result.ok);
        CHECK(result.policy.has_value());
        CHECK(result.policy->max_records.has_value() && *result.policy->max_records == 500);
        CHECK(result.policy->max_age.has_value() &&
              *result.policy->max_age == std::chrono::seconds(60));
    }
    {
        // A typo must be reported, not silently treated as "no policy" --
        // that would leave the caller thinking compaction was configured
        // when it was not.
        const char* argv[] = {"prog", "book.log", "--auto-compact-recrods=5"};
        auto result = parseCompactionPolicyArgs(3, const_cast<char**>(argv), 2);
        CHECK(!result.ok);
        CHECK(!result.error.empty());
    }
    std::cout << "test_parse_compaction_policy_args passed\n";
}

}  // namespace

int main() {
    test_basic_match();
    test_partial_fill_rests_remainder();
    test_price_priority_beats_time();
    test_time_priority_within_same_price();
    test_cancel_removes_order();
    test_market_order_sweeps_multiple_levels();
    test_market_order_never_rests();
    test_limit_order_does_not_cross_worse_price();
    test_duplicate_id_rejected_and_book_untouched();
    test_duplicate_id_rejected_for_market_order();
    test_id_is_reusable_once_no_longer_resting();
    test_fully_filled_incoming_order_does_not_reserve_its_id();
    test_arithmetic_price_crosses_literal_price();
    test_same_price_is_a_single_level();
    test_sub_unit_prices_are_exact();
    test_unfilled_reports_rested_amount_for_limit_order();
    test_unfilled_reports_discarded_amount_for_market_order();
    test_unfilled_is_full_quantity_on_rejection();
    test_modify_shrink_keeps_time_priority();
    test_modify_grow_surrenders_time_priority();
    test_modify_reprice_surrenders_time_priority();
    test_modify_into_a_crossing_price_trades();
    test_modify_rejects_unknown_and_zero_quantity();
    test_modify_leaves_order_cancellable_and_accounted();
    test_modify_a_fully_filled_order_is_rejected();
    test_self_trade_cancel_oldest_removes_resting_order();
    test_self_trade_cancel_oldest_continues_into_next_order();
    test_self_trade_cancel_newest_cancels_incoming_without_resting_it();
    test_cancel_newest_keeps_fills_made_before_the_self_match();
    test_different_participants_trade_normally();
    test_anonymous_orders_are_exempt_from_self_trade_prevention();
    test_self_trade_prevention_applies_to_market_orders();
    test_modify_inherits_the_participant();
    test_engine_rejects_orders_for_unregistered_symbols();
    test_engine_add_symbol_is_idempotent();
    test_engine_books_do_not_interact();
    test_engine_routes_cancel_and_modify_by_symbol();
    test_engine_resting_count_aggregates_across_books();
    test_engine_propagates_self_trade_policy_to_new_books();
    test_replay_reproduces_engine_state();
    test_log_records_rejected_requests_and_replays_them_as_no_ops();
    test_replay_does_not_append_to_an_attached_log();
    test_replay_stops_at_a_torn_final_record();
    test_replay_stops_on_a_sequence_gap();
    test_symbols_must_be_a_single_token();
    test_replay_preserves_participants();
    test_snapshot_round_trip_preserves_state_and_queue_order();
    test_snapshot_is_deterministic_for_identical_state();
    test_truncated_snapshot_is_rejected();
    test_snapshot_of_unknown_version_is_rejected();
    test_compaction_recovers_snapshot_plus_log_tail();
    test_recovery_survives_a_crash_between_snapshot_and_truncation();
    test_wire_round_trips_every_request_type();
    test_framing_is_identical_at_every_possible_split();
    test_framing_yields_nothing_until_a_frame_is_complete();
    test_framing_rejects_bad_version_and_oversized_payloads();
    test_decode_rejects_wrong_payload_length();
    test_encode_rejects_an_over_long_symbol();
    test_response_round_trips_with_trades();
    test_apply_request_reports_rejections_over_the_wire();
    test_authenticate_round_trips_the_raw_token();
    test_authenticate_with_an_empty_token_round_trips();
    test_apply_request_does_not_understand_authenticate();
    test_reject_reason_describe_covers_auth_reasons();
    test_recover_from_nonexistent_log_starts_empty_and_logs_from_zero();
    test_recover_from_existing_log_replays_and_continues_sequence();
    test_recover_from_snapshot_and_log_tail_on_disk();
    test_recover_refuses_a_truncated_log_on_disk();
    test_recover_refuses_an_incomplete_snapshot_on_disk();
    test_compact_writes_snapshot_and_truncates_log();
    test_auto_compactor_triggers_on_record_count();
    test_auto_compactor_triggers_on_age();
    test_auto_compactor_maybe_compact_resets_after_success();
    test_parse_compaction_policy_args();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
