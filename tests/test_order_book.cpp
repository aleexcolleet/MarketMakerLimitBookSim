// Tests for the order book.
//
// Covers what is currently implemented: resting limit orders, cancellation,
// and the query surface. Matching is not implemented yet, so there are no
// matching tests — a test that does not exist is more honest than a test
// that passes for the wrong reason.

#include "mms/order_book.hpp"

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

int checks_run = 0;
std::vector<std::string> failures;

void check(bool ok, const char* expr, const char* file, int line) {
    ++checks_run;
    if (!ok) {
        failures.push_back(std::string(file) + ":" + std::to_string(line) + "  " + expr);
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

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

    if (failures.empty()) {
        std::printf("%d checks passed\n", checks_run);
        return 0;
    }

    std::printf("%zu of %d checks FAILED\n", failures.size(), checks_run);
    for (const auto& f : failures) std::printf("  %s\n", f.c_str());
    return 1;
}
