# Market Maker Limit Book Simulator

A price-time priority limit order book and matching engine in C++17, built to measure where a
market maker's P&L actually comes from — and where it leaks.

**Status:** the matching engine is complete and property-tested. The simulation layer on top of it
— synthetic order flow, a quoting agent, and P&L attribution — is in progress. The roadmap below
is explicit about what does not exist yet.

## Why

A market maker does not forecast price. It quotes two-sided prices, earns the spread, and manages
the inventory it accumulates. Its main enemy is **adverse selection**: the counterparties who trade
against it are, on average, better informed at the moment they trade, so the quotes that get hit
are disproportionately the wrong ones.

That makes a market maker's P&L a mix of two opposing components:

- **Spread capture** — positive, earned on every round trip
- **Adverse selection** — negative, paid to informed flow

Gross P&L hides both. The end goal of this project is to separate them, which is the only way to
tell whether a quoting strategy is genuinely profitable or merely lucky in a trending sample.

## Build and test

```bash
cmake -B build
cmake --build build -j
./build/mms_tests
```

Requires CMake ≥ 3.16 and a C++17 compiler. No external dependencies.

With sanitizers:

```bash
cmake -B build-asan -DMMS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j && ./build-asan/mms_tests
```

**5,248 assertions**, clean in Release under `-Werror` and under AddressSanitizer + UBSan.

## What works

**Matching.** Incoming orders cross against the opposite side, best price first and FIFO within a
level, executing at the **resting** order's price rather than the aggressor's. Limit orders rest
their remainder; market and IOC orders match but never rest.

**Partial fills retain queue position.** Not by special-casing — the front order is decremented and
not popped, so nothing reorders the queue. The bug would be adding code, not omitting it.

**Cancellation** from any queue position, with the price level erased once it empties, and `false`
returned for unknown ids rather than an exception. In a real venue cancels race against fills
constantly, so cancelling a filled order is normal operation, not an error.

**Queries** — `best_bid`, `best_ask`, `top_of_book`, `size_at`, `total_quantity`, `depth` — with
level totals and a live-order count maintained incrementally, so they are O(1).

Not yet implemented: order modification, self-trade prevention, auctions.

## Design

```
include/mms/
  types.hpp        domain types — integer tick prices, orders, trades
  order_book.hpp   public interface, no implementation detail exposed
src/
  order_book.cpp   containers and matching, behind a pimpl
tests/
  test_order_book.cpp
```

Four decisions worth stating up front; the rest, with their alternatives and costs, are in
[docs/DESIGN.md](docs/DESIGN.md).

**Prices are integers, in ticks — never floating point.** Binary floating point cannot represent
0.1 exactly, so price equality (tested on every incoming order) becomes unreliable, and rounding
error accumulates across fills until P&L is fiction. `TopOfBook::mid_x2()` returns twice the mid
for the same reason: with a one-tick spread, integer division would silently lose half a tick,
which is larger than the edge being measured.

**Bids and asks use opposite comparators.** `std::greater<Price>` on bids, `std::less<Price>` on
asks, so `begin()` is the best price on *both* sides and the matching loop never branches on which
side it is walking. The cost is that the two maps are different types, so shared logic is a
template — compile-time polymorphism rather than virtual dispatch on the order-entry path.

**The engine is a deterministic core.** No I/O, no logging, no wall clock — timestamps are a field
on `Order`, supplied by the caller. A book that reads the clock produces different output from
identical input, which makes debugging archaeology. Trades are emitted through a callback rather
than a returned container, so nothing is allocated on the common path where an order rests without
trading.

**The container choice is deliberately provisional.** `std::map` was chosen because getting
matching semantics right matters more at this stage than memory layout; a flat array indexed by
price offset is the expected replacement once there are benchmarks to justify it. Recorded as D2.

## Tests

The suite is the specification. Beyond the obvious cases it covers two things worth calling out:

**Partial fill keeps queue position.** Order 1 is partially filled, order 3 joins the same level
behind it, the level is swept — and order 1's remainder must fill before order 3. This is the case
most order book implementations get wrong.

**Property tests over crossing order flow.** Across thousands of randomly generated orders, the
book must never cross itself (`bid < ask` always), and quantity must be conserved:
`submitted == resting + 2 × traded`. The factor of two is because each trade consumes a lot from
the aggressor *and* one from a resting order, both of which were counted in the submitted total.

## Roadmap

- [x] Domain types, build system, test suite
- [x] Order resting, cancellation, depth and query surface
- [x] Matching engine — limit, market and IOC orders, partial fills, price-time priority
- [ ] Synthetic order flow: Poisson arrivals, informed and uninformed participants trading against
      a latent true value
- [ ] Market-making agent: two-sided quoting, inventory skew, position limits
- [ ] P&L attribution: markout-based decomposition into spread capture and adverse selection
- [ ] Throughput benchmarks and profiling

## Licence

MIT
