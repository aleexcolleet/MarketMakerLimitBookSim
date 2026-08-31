// Tests for the order book.
//
// Covers what is currently implemented: resting limit orders, cancellation,
// and the query surface. Matching is not implemented yet, so there are no
// matching tests — a test that does not exist is more honest than a test
// that passes for the wrong reason.

#include "mms/order_book.hpp"
#include "check.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace mms;

// ---------------------------------------------------------------------------
//  Minimal check harness. No external dependencies, so this builds anywhere
//  with a compiler and CMake.
// ---------------------------------------------------------------------------

namespace {

// Builds a limit order. Keeps the tests reading like order flow.
Order limit_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0) {
    Order o;
    o.id        = id;
    o.side      = side;
    o.type      = OrderType::Limit;
    o.price     = price;
    o.quantity  = qty;
    o.remaining = qty;
    o.timestamp = ts;
    return o;
}

Order market_order(OrderId id, Side side, Quantity qty, Timestamp ts = 0) {
    Order o = limit_order(id, side, 0, qty, ts);
    o.type = OrderType::Market;
    return o;
}

Order ioc_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0) {
    Order o = limit_order(id, side, price, qty, ts);
    o.type = OrderType::IOC;
    return o;
}

// Records the trades the book emits so tests can assert on them.
struct Tape {
    std::vector<Trade> trades;

    void attach(OrderBook& b) {
        b.set_trade_callback([this](const Trade& t) { trades.push_back(t); });
    }
    std::size_t count() const { return trades.size(); }
    const Trade& operator[](std::size_t i) const { return trades.at(i); }
    Quantity total_quantity() const {
        Quantity q = 0;
        for (const auto& t : trades) q += t.quantity;
        return q;
    }
};

// ---------------------------------------------------------------------------
//  An empty book
// ---------------------------------------------------------------------------

void empty_book_reports_no_prices() {
    OrderBook b;
    CHECK(b.best_bid() == kInvalidPrice);
    CHECK(b.best_ask() == kInvalidPrice);
    CHECK(b.order_count() == 0);
    CHECK(b.total_quantity(Side::Buy) == 0);
    CHECK(b.total_quantity(Side::Sell) == 0);
    CHECK(!b.top_of_book().is_two_sided());
    CHECK(b.depth(Side::Buy, 5).empty());
}

// ---------------------------------------------------------------------------
//  Resting orders
// ---------------------------------------------------------------------------

void single_order_rests() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 50));

    CHECK(b.best_bid() == 10000);
    CHECK(b.best_ask() == kInvalidPrice);
    CHECK(b.size_at(Side::Buy, 10000) == 50);
    CHECK(b.order_count() == 1);
    CHECK(b.contains(1));
}

void best_bid_is_highest_best_ask_is_lowest() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy,   9900, 10));
    b.submit(limit_order(2, Side::Buy,  10100, 10));
    b.submit(limit_order(3, Side::Buy,  10000, 10));
    b.submit(limit_order(4, Side::Sell, 10500, 10));
    b.submit(limit_order(5, Side::Sell, 10300, 10));
    b.submit(limit_order(6, Side::Sell, 10400, 10));

    CHECK(b.best_bid() == 10100);
    CHECK(b.best_ask() == 10300);
    CHECK(b.top_of_book().spread() == 200);
    CHECK(b.top_of_book().mid_x2() == 20400);
    CHECK(b.total_quantity(Side::Buy) == 30);
    CHECK(b.total_quantity(Side::Sell) == 30);
}

void quantity_accumulates_at_a_price_level() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10, 1));
    b.submit(limit_order(2, Side::Buy, 10000, 25, 2));
    b.submit(limit_order(3, Side::Buy, 10000,  5, 3));

    CHECK(b.size_at(Side::Buy, 10000) == 40);
    CHECK(b.order_count() == 3);
    CHECK(b.top_of_book().bid_size == 40);
}

void size_at_an_empty_level_is_zero() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10));

    CHECK(b.size_at(Side::Buy, 9999) == 0);
    CHECK(b.size_at(Side::Sell, 10000) == 0);   // right price, wrong side
}

void duplicate_order_id_is_rejected() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10));
    b.submit(limit_order(1, Side::Buy, 10100, 99));   // same id

    CHECK(b.order_count() == 1);
    CHECK(b.best_bid() == 10000);
    CHECK(b.size_at(Side::Buy, 10100) == 0);
}

void non_positive_quantity_is_rejected() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 0));
    b.submit(limit_order(2, Side::Buy, 10000, -5));

    CHECK(b.order_count() == 0);
    CHECK(b.best_bid() == kInvalidPrice);
}

// ---------------------------------------------------------------------------
//  Cancellation
// ---------------------------------------------------------------------------

void cancel_removes_an_order() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10, 1));
    b.submit(limit_order(2, Side::Buy, 10000, 30, 2));

    CHECK(b.cancel(1));
    CHECK(b.size_at(Side::Buy, 10000) == 30);
    CHECK(b.order_count() == 1);
    CHECK(!b.contains(1));
    CHECK(b.contains(2));
}

void cancel_works_from_any_queue_position() {
    OrderBook b;
    b.submit(limit_order(1, Side::Sell, 10000, 10, 1));
    b.submit(limit_order(2, Side::Sell, 10000, 20, 2));
    b.submit(limit_order(3, Side::Sell, 10000, 30, 3));

    CHECK(b.cancel(2));                              // middle
    CHECK(b.size_at(Side::Sell, 10000) == 40);
    CHECK(b.cancel(3));                              // back
    CHECK(b.size_at(Side::Sell, 10000) == 10);
    CHECK(b.cancel(1));                              // front, and last
    CHECK(b.best_ask() == kInvalidPrice);
}

void cancelling_the_last_order_removes_the_price_level() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10100, 10));
    b.submit(limit_order(2, Side::Buy, 10000, 10));

    CHECK(b.cancel(1));
    CHECK(b.best_bid() == 10000);
    CHECK(b.size_at(Side::Buy, 10100) == 0);
    CHECK(b.depth(Side::Buy, 5).size() == 1);
}

void cancelling_an_unknown_id_returns_false() {
    // Cancels race against fills in a real system, so a cancel for an order
    // that no longer exists is normal operation, not an error.
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10));

    CHECK(!b.cancel(999));
    CHECK(!b.cancel(0));
    CHECK(b.order_count() == 1);

    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));      // second cancel of the same id
    CHECK(!b.cancel(1));
    CHECK(b.order_count() == 0);
}

// ---------------------------------------------------------------------------
//  Depth and bookkeeping
// ---------------------------------------------------------------------------

void depth_returns_levels_best_first() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy, 10000, 10, 1));
    b.submit(limit_order(2, Side::Buy,  9800, 20, 2));
    b.submit(limit_order(3, Side::Buy,  9900, 30, 3));
    b.submit(limit_order(4, Side::Buy,  9900,  5, 4));

    const auto d = b.depth(Side::Buy, 3);

    CHECK(d.size() == 3);
    CHECK(d[0].price == 10000 && d[0].quantity == 10 && d[0].order_count == 1);
    CHECK(d[1].price ==  9900 && d[1].quantity == 35 && d[1].order_count == 2);
    CHECK(d[2].price ==  9800);
}

void ask_depth_is_ascending() {
    OrderBook b;
    b.submit(limit_order(1, Side::Sell, 10300, 10));
    b.submit(limit_order(2, Side::Sell, 10100, 10));
    b.submit(limit_order(3, Side::Sell, 10200, 10));

    const auto d = b.depth(Side::Sell, 3);

    CHECK(d.size() == 3);
    CHECK(d[0].price == 10100);
    CHECK(d[1].price == 10200);
    CHECK(d[2].price == 10300);
}

void depth_is_truncated_to_requested_levels() {
    OrderBook b;
    for (Price p = 9900; p < 9910; ++p) {
        b.submit(limit_order(static_cast<OrderId>(p), Side::Buy, p, 10));
    }

    CHECK(b.depth(Side::Buy, 3).size() == 3);
    CHECK(b.depth(Side::Buy, 100).size() == 10);
    CHECK(b.depth(Side::Buy, 0).empty());
    CHECK(b.depth(Side::Sell, 5).empty());
}

void clear_empties_the_book() {
    OrderBook b;
    b.submit(limit_order(1, Side::Buy,  10000, 10));
    b.submit(limit_order(2, Side::Sell, 10100, 10));

    b.clear();

    CHECK(b.order_count() == 0);
    CHECK(b.best_bid() == kInvalidPrice);
    CHECK(b.best_ask() == kInvalidPrice);
    CHECK(!b.contains(1));
    CHECK(b.total_quantity(Side::Sell) == 0);
}

// ---------------------------------------------------------------------------
//  Cached aggregates must agree with the orders that are actually resting.
//  These are the invariants that break first when a mutation path forgets to
//  update the cached totals.
// ---------------------------------------------------------------------------

void cached_totals_stay_consistent_under_churn() {
    OrderBook b;
    std::uint64_t seed = 42;
    auto rnd = [&seed](std::uint64_t n) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33) % n;
    };

    for (OrderId i = 1; i <= 2000; ++i) {
        const Side  side = rnd(2) ? Side::Buy : Side::Sell;
        const Price px   = static_cast<Price>(9950 + rnd(100));
        b.submit(limit_order(i, side, px, static_cast<Quantity>(1 + rnd(20)),
                             static_cast<Timestamp>(i)));
        if (rnd(3) == 0) b.cancel(static_cast<OrderId>(1 + rnd(i)));
    }

    // total_quantity() sums cached level totals; depth() reports them per
    // level. Summing every level must reproduce the same number.
    for (const Side side : {Side::Buy, Side::Sell}) {
        Quantity from_depth = 0;
        std::size_t orders_in_depth = 0;
        for (const auto& lvl : b.depth(side, 10000)) {
            from_depth += lvl.quantity;
            orders_in_depth += lvl.order_count;
            CHECK(lvl.quantity > 0);          // empty levels must be erased
            CHECK(lvl.order_count > 0);
        }
        CHECK(from_depth == b.total_quantity(side));
        (void)orders_in_depth;
    }

    // Every live order must be reachable through the id index, and the count
    // of live orders must match what the levels actually hold.
    std::size_t orders_in_levels = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const auto& lvl : b.depth(side, 10000)) orders_in_levels += lvl.order_count;
    }
    CHECK(orders_in_levels == b.order_count());
}

void moved_book_keeps_its_contents() {
    OrderBook a;
    a.submit(limit_order(1, Side::Buy, 10000, 42));

    OrderBook b = std::move(a);
    CHECK(b.order_count() == 1);
    CHECK(b.best_bid() == 10000);
    CHECK(b.size_at(Side::Buy, 10000) == 42);

    OrderBook c;
    c = std::move(b);
    CHECK(c.order_count() == 1);
    CHECK(c.contains(1));
}

// ---------------------------------------------------------------------------
//  Matching
// ---------------------------------------------------------------------------

void marketable_limit_crosses_and_trades() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10000, 50, 1));
    const Quantity filled = b.submit(limit_order(2, Side::Buy, 10000, 30, 2));

    CHECK(filled == 30);
    CHECK(tape.count() == 1);
    CHECK(tape[0].price == 10000);
    CHECK(tape[0].quantity == 30);
    CHECK(tape[0].resting_id == 1);
    CHECK(tape[0].aggressor_id == 2);
    CHECK(tape[0].aggressor_side == Side::Buy);
    CHECK(b.size_at(Side::Sell, 10000) == 20);   // remainder still rests
    CHECK(b.best_bid() == kInvalidPrice);        // buyer fully filled, nothing rests
    CHECK(b.order_count() == 1);
}

void trade_executes_at_the_resting_price() {
    // The resting order set the terms. A buyer willing to pay 10500 who lifts
    // an offer at 10000 pays 10000 and keeps the price improvement.
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10000, 10));
    b.submit(limit_order(2, Side::Buy,  10500, 10));

    CHECK(tape.count() == 1);
    CHECK(tape[0].price == 10000);
    CHECK(b.order_count() == 0);
}

void non_marketable_order_rests_without_trading() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10100, 10));
    const Quantity filled = b.submit(limit_order(2, Side::Buy, 10000, 10));

    CHECK(filled == 0);
    CHECK(tape.count() == 0);
    CHECK(b.best_bid() == 10000);
    CHECK(b.best_ask() == 10100);
}

void aggressor_sweeps_levels_best_price_first() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10200, 10));
    b.submit(limit_order(2, Side::Sell, 10000, 10));
    b.submit(limit_order(3, Side::Sell, 10100, 10));

    const Quantity filled = b.submit(limit_order(4, Side::Buy, 10200, 25));

    CHECK(filled == 25);
    CHECK(tape.count() == 3);
    CHECK(tape[0].price == 10000);   // best price first
    CHECK(tape[1].price == 10100);
    CHECK(tape[2].price == 10200);
    CHECK(tape[2].quantity == 5);    // partial at the last level
    CHECK(b.size_at(Side::Sell, 10200) == 5);
}

void fifo_within_a_price_level() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10000, 10, 1));
    b.submit(limit_order(2, Side::Sell, 10000, 10, 2));
    b.submit(limit_order(3, Side::Sell, 10000, 10, 3));

    b.submit(limit_order(4, Side::Buy, 10000, 15, 4));

    CHECK(tape.count() == 2);
    CHECK(tape[0].resting_id == 1);      // earliest fills first
    CHECK(tape[0].quantity == 10);
    CHECK(tape[1].resting_id == 2);
    CHECK(tape[1].quantity == 5);        // partial
    CHECK(b.size_at(Side::Sell, 10000) == 15);
    CHECK(!b.contains(1));
    CHECK(b.contains(2));
    CHECK(b.contains(3));
    CHECK(b.order_count() == 2);
}

void partial_fill_keeps_queue_position() {
    // Order 1 is partially filled, then order 3 joins the same level.
    // Order 1's remainder must still fill before order 3.
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10000, 20, 1));
    b.submit(limit_order(2, Side::Buy,  10000,  5, 2));   // fills 5 of order 1
    b.submit(limit_order(3, Side::Sell, 10000, 10, 3));   // joins behind order 1
    b.submit(limit_order(4, Side::Buy,  10000, 20, 4));   // sweeps the level

    CHECK(tape.count() == 3);
    CHECK(tape[1].resting_id == 1);
    CHECK(tape[1].quantity == 15);   // order 1's remainder, still at the front
    CHECK(tape[2].resting_id == 3);
    CHECK(tape[2].quantity == 5);
}

void sell_side_matching_is_symmetric() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Buy, 10100, 10, 1));
    b.submit(limit_order(2, Side::Buy, 10000, 10, 2));

    const Quantity filled = b.submit(limit_order(3, Side::Sell, 10000, 15, 3));

    CHECK(filled == 15);
    CHECK(tape[0].price == 10100);   // best bid first
    CHECK(tape[1].price == 10000);
    CHECK(tape[0].aggressor_side == Side::Sell);
    CHECK(b.size_at(Side::Buy, 10000) == 5);
}

// ---------------------------------------------------------------------------
//  Market and IOC orders
// ---------------------------------------------------------------------------

void market_order_takes_best_available_price() {
    OrderBook b; Tape tape; tape.attach(b);

    b.submit(limit_order(1, Side::Sell, 10000, 10));
    b.submit(limit_order(2, Side::Sell, 10100, 10));

    const Quantity filled = b.submit(market_order(3, Side::Buy, 15));

    CHECK(filled == 15);
    CHECK(tape[0].price == 10000);
    CHECK(tape[1].price == 10100);
    CHECK(tape[1].quantity == 5);
}

void market_order_never_rests() {
    OrderBook b;
    b.submit(limit_order(1, Side::Sell, 10000, 10));

    const Quantity filled = b.submit(market_order(2, Side::Buy, 50));

    CHECK(filled == 10);
    CHECK(b.order_count() == 0);     // the unfilled 40 is discarded
    CHECK(!b.contains(2));
}

void market_order_into_empty_book_does_nothing() {
    OrderBook b; Tape tape; tape.attach(b);

    const Quantity filled = b.submit(market_order(1, Side::Buy, 10));

    CHECK(filled == 0);
    CHECK(tape.count() == 0);
    CHECK(b.order_count() == 0);
}

void ioc_fills_what_it_can_and_discards_the_rest() {
    OrderBook b;
    b.submit(limit_order(1, Side::Sell, 10000, 10));

    const Quantity filled = b.submit(ioc_order(2, Side::Buy, 10000, 30));

    CHECK(filled == 10);
    CHECK(b.order_count() == 0);
    CHECK(b.best_bid() == kInvalidPrice);
}

// ---------------------------------------------------------------------------
//  Invariants under crossing order flow
// ---------------------------------------------------------------------------

void book_never_crosses_itself() {
    OrderBook b;
    std::uint64_t seed = 7;
    auto rnd = [&seed](std::uint64_t n) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33) % n;
    };

    for (OrderId i = 1; i <= 5000; ++i) {
        const Side  side = rnd(2) ? Side::Buy : Side::Sell;
        const Price px   = static_cast<Price>(9950 + rnd(100));
        b.submit(limit_order(i, side, px, static_cast<Quantity>(1 + rnd(20)),
                             static_cast<Timestamp>(i)));

        const auto tob = b.top_of_book();
        if (tob.is_two_sided()) CHECK(tob.bid_price < tob.ask_price);
    }
}

void quantity_is_conserved() {
    // Every lot submitted ends up either traded or resting. Each trade consumes
    // one lot from the aggressor and one from a resting order, so traded volume
    // is counted twice against what was submitted.
    OrderBook b; Tape tape; tape.attach(b);
    std::uint64_t seed = 99;
    auto rnd = [&seed](std::uint64_t n) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33) % n;
    };

    Quantity submitted = 0;
    for (OrderId i = 1; i <= 3000; ++i) {
        const Side     side = rnd(2) ? Side::Buy : Side::Sell;
        const Price    px   = static_cast<Price>(9980 + rnd(40));
        const Quantity q    = static_cast<Quantity>(1 + rnd(15));
        submitted += q;
        b.submit(limit_order(i, side, px, q, static_cast<Timestamp>(i)));
    }

    const Quantity resting = b.total_quantity(Side::Buy) + b.total_quantity(Side::Sell);
    CHECK(submitted == resting + 2 * tape.total_quantity());
}

}  // anonymous namespace

int main() {
    empty_book_reports_no_prices();
    single_order_rests();
    best_bid_is_highest_best_ask_is_lowest();
    quantity_accumulates_at_a_price_level();
    size_at_an_empty_level_is_zero();
    duplicate_order_id_is_rejected();
    non_positive_quantity_is_rejected();
    cancel_removes_an_order();
    cancel_works_from_any_queue_position();
    cancelling_the_last_order_removes_the_price_level();
    cancelling_an_unknown_id_returns_false();
    depth_returns_levels_best_first();
    ask_depth_is_ascending();
    depth_is_truncated_to_requested_levels();
    clear_empties_the_book();
    cached_totals_stay_consistent_under_churn();
    moved_book_keeps_its_contents();

    marketable_limit_crosses_and_trades();
    trade_executes_at_the_resting_price();
    non_marketable_order_rests_without_trading();
    aggressor_sweeps_levels_best_price_first();
    fifo_within_a_price_level();
    partial_fill_keeps_queue_position();
    sell_side_matching_is_symmetric();

    market_order_takes_best_available_price();
    market_order_never_rests();
    market_order_into_empty_book_does_nothing();
    ioc_fills_what_it_can_and_discards_the_rest();

    book_never_crosses_itself();
    quantity_is_conserved();

    return check_harness::report();
}
