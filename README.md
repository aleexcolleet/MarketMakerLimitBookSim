# Market Maker Simulator

A limit order book, matching engine and simulated market-making agent in C++, built to
measure where a market maker's P&L actually comes from — and where it leaks.

> **Status: in development.** Phase 1 (matching engine) is under way. This README describes
> what exists; the roadmap below is explicitly not yet built. See [docs/DESIGN.md](docs/DESIGN.md)
> for the design decisions and their justifications.

## Why this project

A market maker does not forecast price. It quotes two-sided prices, earns the spread, and
manages the inventory it accumulates. Its main enemy is **adverse selection**: the counterparties
who trade against it are, on average, better informed at the moment they trade, so the quotes
that get hit are disproportionately the wrong ones.

That makes a market maker's P&L a mix of two opposing components:

- **Spread capture** — positive, earned on every round trip
- **Adverse selection** — negative, paid to informed flow

Gross P&L hides both. This project separates them, which is the only way to tell whether a
quoting strategy is genuinely profitable or merely lucky in a trending sample.

## Build

```bash
cmake -B build
cmake --build build -j
./build/tests/mms_tests
```

Requires CMake ≥ 3.16 and a C++17 compiler. No external dependencies.

Build with sanitizers while developing:

```bash
cmake -B build-asan -DMMS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j && ./build-asan/tests/mms_tests
```

## Architecture

```
include/mms/         public headers
  types.hpp          domain types — integer prices, orders, trades
  order_book.hpp     price-time priority book + matching engine
src/                 implementation
tests/               specification-as-tests, no external framework
```

### Design decisions

**Prices are integers, in ticks.** Never doubles. Binary floating point cannot represent 0.1,
so equality comparisons break; rounding accumulates across millions of fills and corrupts P&L;
and integer comparison is faster in the matching hot path.

**Matching is price-time priority.** Better prices fill first; within a price, earlier arrivals
fill first. Trades execute at the *resting* order's price — the resting order set the terms.

Further decisions, with their trade-offs, are recorded in [docs/DESIGN.md](docs/DESIGN.md).

## Roadmap

- [x] Domain types, build system, test harness
- [ ] **Phase 1** — matching engine: limit/market/IOC orders, cancels, partial fills, depth
- [ ] **Phase 2** — synthetic order flow: Poisson arrivals, informed and uninformed participants
      trading against a latent true value
- [ ] **Phase 3** — market-making agent: two-sided quoting, inventory skew, position limits
- [ ] **Phase 4** — P&L attribution: markout-based decomposition into spread capture and
      adverse selection, by horizon
- [ ] **Phase 5** — throughput benchmarks and profiling

## Tests

The test suite is the specification. It covers resting and matching behaviour, FIFO queue
priority (including the case where a partially filled order must keep its position), market
and IOC semantics, depth snapshots, and two property-style invariants: the book must never
cross itself, and quantity must be conserved across arbitrary order flow.

```
./build/tests/mms_tests
```

## Licence

MIT
