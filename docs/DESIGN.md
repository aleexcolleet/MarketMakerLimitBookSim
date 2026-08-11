# Design decisions

A record of the choices made building this system, the alternatives considered, and why each
one won.

> **How to use this file.** Every entry reflects a real decision taken during development. Read
> each one and check you agree — where the stated reasoning isn't the reasoning you actually
> hold, change it. This document is only worth something if you can defend every line without
> notes.

---

## What exists so far

```
Layer 3   drivers, benchmarks, output          ← not built
Layer 2   flow generator, market maker         ← not built
Layer 1   order book / matching engine         ← interface + lifecycle done,
             ↓ depends on                         matching in progress
Layer 0   types.hpp                            ← done
```

Dependencies point downward only. Layer 1 knows nothing about Layer 3. See D10.

| File | Status |
|---|---|
| `include/mms/types.hpp` | Complete — domain vocabulary |
| `include/mms/order_book.hpp` | Complete — public interface |
| `src/order_book.cpp` | Data structures and lifecycle done; matching stubbed |
| `tests/` | Not started |

---

## D1 — Prices are integers in ticks, not floating point

**Decision.** `Price = std::int64_t`, denominated in ticks. `Quantity = std::int64_t`.

**Alternatives.** `double`; a fixed-point decimal class; a strong typedef wrapping the integer.

**Why.**

1. **Representation.** Binary floating point cannot represent 0.1 exactly. A matching engine
   tests price equality on every incoming order, so unreliable equality is disqualifying.
2. **Accumulation.** Rounding error compounds across fills and silently corrupts P&L. The
   failure mode is the worst kind — it looks like a strategy that doesn't work, not like a bug.
3. **Speed.** Integer comparison is cheaper than floating point in the matching hot path.

Every production exchange does this.

**Cost.** Callers must know the tick size to render human-readable prices. Mid prices are not
representable when the spread is an odd number of ticks — hence `TopOfBook::mid_x2()`, which
returns twice the mid so nothing is lost to integer division. The general rule this enforces:
**never round in the middle of an accumulation.**

**Known limitation.** `Price` and `Quantity` are type *aliases*, not distinct types, so
`price = quantity` compiles. A strong typedef would make that a compile error at the cost of
boilerplate on every arithmetic operation. Judged not worth it at this scale — but it is a real
hole in the type safety, not an oversight.

**Signedness.** `Quantity` is signed despite never being negative, because quantities are
subtracted constantly and inventory genuinely goes negative when short. Mixing signed and
unsigned in an expression silently converts to unsigned, so `3u - 5u` is a large positive number
rather than `-2`. Keeping everything signed makes that bug unwriteable. `OrderId` is unsigned
because it is a label, never arithmetic.

---

## D2 — Price levels in `std::map`, deliberately provisional

**Decision.** `std::map<Price, PriceLevel>` per side, with the explicit intention to revisit
after benchmarking.

**Alternatives considered.**

- **`std::vector<PriceLevel>` indexed by `price - base_price`.** O(1) lookup, contiguous memory,
  excellent cache behaviour. This is what most production books use, justified empirically: real
  instruments trade in a narrow band intraday, so reserving ±1000 ticks costs tens of KB and
  covers essentially every price seen. Costs memory proportional to the band rather than to live
  levels, and needs a policy for prices outside it.
- **Intrusive linked list of levels + hash map for lookup.** O(1) both ways, more code, more
  pointer chasing.

**Why `std::map` for now.** The current objective is correct matching semantics, not memory
layout. `std::map` is materially harder to get wrong: it handles any price range, its iterators
survive insertion and erasure of *other* elements, and best-price lookup is `begin()`. Choosing
the fast structure first would mean debugging matching logic and memory layout simultaneously.

**Cost, accepted knowingly.** O(log n) rather than O(1) lookup. One heap allocation per price
level. Nodes scattered in memory, so walking levels is a series of cache misses.

**What would change this decision.** Benchmark results in Phase 5. The expectation is that
per-level allocation dominates and a flat vector wins. The point of deferring is to make that
change on measured evidence rather than assumption.

---

## D3 — Cancelling from mid-queue: unresolved

**The problem.** Orders within a price level are FIFO, so the natural container is a queue. But
cancels target arbitrary positions, not just the front — and in real markets cancels vastly
outnumber fills. `std::deque::erase` from the middle is O(n), because subsequent elements shift.

**Current state.** `std::deque` with immediate erase. O(n) cancel, accepted for now.

**Alternatives.**

- **Lazy deletion.** Mark the order dead, skip it while matching, sweep periodically. O(1)
  cancel; costs memory held by dead orders and a more complex match loop.
- **Intrusive doubly-linked list + `unordered_map<OrderId, node*>`.** O(1) cancel *and* O(1)
  removal. One allocation per order.

**Why unresolved.** The right answer depends on the cancel-to-fill ratio of the simulated flow,
which the Phase 2 generator hasn't produced yet. Deciding now would be guessing.

**Recorded because** an interviewer asking "what happens when I cancel an order in the middle of
the queue?" is testing whether the cost was noticed. It was.

---

## D4 — Pimpl in `OrderBook`

**Decision.** `OrderBook` holds an opaque `Impl*`. The header exposes no container types.

**Why.** The public interface is fixed by the tests; the internal representation is explicitly
provisional (D2, D3). Pimpl means changing it recompiles one translation unit rather than
everything downstream, and keeps `<map>`, `<deque>` and `<unordered_map>` out of the public API
so callers don't pay to compile them.

**Cost.** One pointer indirection per call, one heap allocation per book. Irrelevant here —
books are constructed once, not in a loop.

**Consequence.** The destructor and move operations must be *defined* in the `.cpp`, where
`Impl` is complete. Letting the compiler generate them would emit `delete` on an incomplete type
at each use site, which is undefined behaviour.

**Alternative not taken.** `std::unique_ptr<Impl>` would remove four of the five special member
declarations. Written manually here to make ownership explicit rather than delegated.

---

## D5 — Trades execute at the resting order's price

**Decision.** When an incoming order crosses, the trade prints at the **resting** order's price,
not the aggressor's.

**Why.** The resting order posted a firm price and accepted the risk of being adversely
selected. The aggressor chose to accept those terms. A buyer willing to pay 105 who lifts an
offer at 100 pays 100 and keeps the price improvement. Standard exchange behaviour, and what
makes market making economically coherent.

---

## D6 — Trades stream through a callback, not a returned container

**Decision.** `submit()` returns the executed `Quantity`; individual trades are delivered via a
`std::function<void(const Trade&)>` sink.

**Alternative.** Return `std::vector<Trade>`.

**Why.**

- **No allocation on the common path.** Most submits rest without trading; returning a vector
  would allocate on every call regardless. `submit()` is the hot path.
- **Streaming.** The consumer sees each trade in order, as it happens.
- **Decoupling.** The book does not know whether the consumer is a test tape, a P&L engine or a
  CSV writer. D10's dependency rule, applied concretely.

**Cost, known.** `std::function` is a type-erased indirect call — not inlinable, roughly the cost
of a virtual call. Incurred once per *trade*, not per instruction, so almost certainly
irrelevant. If benchmarking says otherwise, the fix is to template the book on the sink type,
which is mechanical.

**Division of labour.** The return value answers *how much*; the callback answers *at what
prices*. Most callers need only the former; only P&L attribution needs the latter.

---

## D7 — Copying deleted, moving kept

**Decision.** Copy constructor and copy assignment are `= delete`. Move constructor and move
assignment are defined and `noexcept`.

**Why delete copying.** An order book represents one venue's state; two identical books is not a
meaningful concept. A copy is therefore almost certainly an accident — most realistically
`void f(OrderBook b)` written instead of `void f(OrderBook& b)`, which would silently deep-copy
the book on every call. Deleting turns an invisible performance bug into a compile error.

**Why keep moving.** "This book now lives over here" is meaningful, and it costs one pointer.

**Why `noexcept` on the moves.** `std::vector` moves its elements on reallocation only if their
move constructor is `noexcept`; otherwise it must copy, since a throwing move mid-realloc would
be unrecoverable. With copying deleted, omitting `noexcept` would make the type unusable in some
standard containers. The keyword changes behaviour, not just performance.

---

## D8 — Cached aggregates: `PriceLevel::total` and `Impl::live_orders`

**Decision.** Each price level caches the sum of its resting quantities; the book caches a count
of live orders.

**Why.** Turns `size_at()` and `order_count()` from O(n) into O(1). Both are called constantly by
tests and, later, by the market-making agent on every quote decision.

**Cost.** Every mutation path must update them: rest, partial fill, full fill, cancel. A cached
value is a promise, and each mutation is an opportunity to break it.

**Invariants this creates**, both of which must hold before and after every public operation:

1. `level.total == sum of remaining over level.queue`
2. `index` contains exactly the ids live in some level — no more, no fewer

**Mitigation.** A test that recomputes both from first principles and asserts they match the
cached values. Nearly every bug in a matching engine is one of these two drifting.

---

## D9 — `RestingOrder` stores only non-derivable data

**Decision.** A resting order stores `id`, `remaining`, `timestamp` and `owner` — not `price`,
`side` or `type`.

**Why.** Price is the key of the level containing it. Side is which map it is in. Only limit
orders rest, so type is implied. Storing them would create a **second source of truth** that can
disagree with the first: a book where an order believes it is at 101 while its level believes
100, with no way to tell which is right.

Smaller structs are a side benefit, not the reason.

---

## D10 — Layered dependencies, not ports and adapters

**Decision.** A functional-core / imperative-shell layering with dependencies pointing inward.
Explicitly *not* hexagonal architecture.

**Why not hexagonal.** Ports and adapters exists to isolate domain logic from infrastructure —
databases, HTTP APIs, message brokers, UIs. This system has none of those, so every port would
have exactly one implementation: architecture as decoration.

More importantly, hexagonal's mechanism is dependency inversion through abstract base classes,
which means **virtual dispatch**. `submit()` is the hot path and the function whose latency this
project exists to measure. Adding non-inlinable indirect calls to it would be deliberately
pessimising the one thing being optimised.

**What is kept — the dependency rule.** Three constraints on the matching engine:

- **No I/O.** No printing, logging or file access. Emit events; let callers decide.
- **No clock.** `Timestamp` is a field on `Order`, supplied from outside. A book that reads the
  wall clock is non-deterministic — the same input produces different output and debugging
  becomes archaeology. Time is data.
- **No allocation in the hot path**, eventually. Not yet true (D2 allocates per level). Design so
  preallocation remains possible.

**Where substitutability is genuinely needed** — swapping market-making strategies to compare
them — the mechanism will be templates, not virtual functions. Same substitutability, resolved
and inlined at compile time.

---

## D11 — Asymmetric comparators for a symmetric matching loop

**Decision.** `bids` uses `std::greater<Price>`; `asks` uses `std::less<Price>`.

**Why.** With bids sorted descending and asks ascending, `begin()` is the **best price on both
sides**. The matching loop walks `opposite.begin()` without ever branching on which side it is
examining — one code path instead of two, so half as many places to get the direction backwards.

**Cost.** `bids` and `asks` are now different *types*, since the comparator is part of a
`std::map`'s type. Shared matching logic therefore has to be a template — compile-time
polymorphism, consistent with D10.

**Note.** The comparator's type argument is the **key** type, not the value type.
`std::map<Price, PriceLevel, std::greater<PriceLevel>>` is a silent error: it names a valid type,
and only fails when the comparator is actually invoked, because C++ instantiates template
members lazily.

---

## Open questions

- **D3** — cancel strategy, pending the cancel-to-fill ratio from the Phase 2 flow generator
- **D2** — container choice, pending Phase 5 benchmarks
- Whether `Price` and `Quantity` should become strong typedefs. Currently judged not worth the
  boilerplate; revisit if a unit-confusion bug actually occurs
- Whether the trade callback should become a template parameter, pending benchmark evidence that
  the indirect call matters

---

## Working notes

*Raw learning notes kept alongside the decisions above.*

### Stage 3 — matching engine

- **`namespace {}`** with no name gives everything internal linkage. Modern replacement for the
  old file-scope `static`.
- **`RestingOrder`** deliberately smaller — see D9.
- **`total`** is a cached sum. Makes `size_at()` O(1) instead of walking the queue. Every path
  that touches `queue` must also update `total`: rest, partial fill, full fill, cancel. See D8.

### Block 3 — `Impl`

The comparators are the important line. `std::map`'s third template parameter decides sort
order, so the map stores pairs in sorted order:

```
Price (ticks) → PriceLevel

9995 → 200 lots
9990 → 400 lots
9985 → 100 lots
```

With `std::greater<Price>` on bids, `begin()` is 9995 — the best bid. With `std::less<Price>` on
asks, `begin()` is the lowest ask — the best ask. Best price is `begin()` on both sides.
