# Stage 3 — The matching engine

**What you'll write:** `src/order_book.cpp`
**Branch:** `feature/matching-engine`
**Assumed knowledge:** Stages 1–2.

This is the stage with a real algorithm. It's also the one an interviewer will actually dig
into, because it's where correctness is subtle and the data structure choice becomes visible.

Build it in four sub-steps, in order. Each ends with something you can test.

---

## Part A — What the algorithm actually is

When an order arrives, exactly two things can happen to it, in this order:

1. **Match** it against the opposite side, as far as it can go
2. **Rest** whatever is left (limit orders only — market and IOC discard the remainder)

That's the whole engine. Everything else is bookkeeping.

The matching step in words:

```
while the incoming order still has quantity
  and the opposite side is not empty
  and the best opposite price is acceptable to us:

      take the best price level on the opposite side
      while that level has orders and we still have quantity:
          fill against the front order (FIFO)
          traded = min(our remaining, their remaining)
          emit a Trade at THEIR price
          reduce both
          if their order is now empty, remove it from the front
      if the level is now empty, erase the level
```

"Acceptable to us" means:

- We're **buying**: their ask ≤ our limit price
- We're **selling**: their bid ≥ our limit price
- We're a **market order**: always acceptable

Three details that are easy to get wrong and that the tests will catch:

- The trade prints at the **resting** order's price
- Within a level, **strictly FIFO** — always the front
- A partially filled resting order **keeps its position** at the front, it does not go to the back

---

## Part B — The containers

### `std::map<Key, Value>`

An ordered associative container — a balanced binary search tree. Keys are unique and kept
sorted, so iteration is in key order and `begin()` is the smallest key.

- `O(log n)` insert, find, erase
- One heap allocation per node
- Iterators stay valid when you insert or erase *other* elements

```cpp
std::map<Price, PriceLevel> asks;   // ascending: begin() is the LOWEST ask ✓
```

For bids you want the opposite order. The third template parameter is the comparator:

```cpp
std::map<Price, PriceLevel, std::greater<Price>> bids;  // descending: begin() is the HIGHEST bid ✓
```

**This is the trick that makes your code symmetric.** With `std::greater` on bids, `begin()` is
the best price on *both* sides. Your matching loop never has to branch on which side it's
walking. Fewer branches, fewer bugs.

Note `bids` and `asks` are now **different types** — the comparator is part of the type. That's
what motivates the template helper in Part E.

### `std::deque<T>`

A double-ended queue. `O(1)` push and pop at *both* ends, and indexable like a vector.

```cpp
std::deque<RestingOrder> queue;
queue.push_back(order);    // new order joins the back
queue.front();             // the order that fills next
queue.pop_front();         // remove it once filled
```

Exactly the FIFO shape a price level needs.

**Its weakness, which is D3:** erasing from the *middle* is `O(n)`, because everything after
has to shift. Cancels target arbitrary positions and, in real markets, cancels vastly outnumber
fills. Live with it for now; note it.

### `std::unordered_map<Key, Value>`

A hash table. `O(1)` average lookup, unordered iteration.

You need this to make `cancel(id)` work at all. Given only an `OrderId`, you'd otherwise have to
scan every level of both sides. Instead, keep an index from id to *where the order lives*:

```cpp
struct Locator { Side side; Price price; };
std::unordered_map<OrderId, Locator> index;
```

`cancel` then becomes: look up the locator, go straight to that level, find the order in the
deque, remove it. The deque scan is the `O(n)` part.

**Rule:** any time you must find something by a key that isn't the container's sort order, you
need a second index. Keeping two structures in sync is a classic source of bugs — every
insert, every erase, every full fill must update both.

---

## Part C — Iterators, and the bug that will bite you

An **iterator** is a generalised pointer. `*it` gives the element, `++it` advances.

```cpp
auto it = asks.begin();      // iterator to the first (best) element
if (it != asks.end()) { ... } // end() is one PAST the last — never dereference it
```

For a `map`, `*it` is a `std::pair<const Key, Value>`. So `it->first` is the price and
`it->second` is the level. The key is `const` because changing it would break the sort order.

### Iterator invalidation

When you modify a container, some iterators may become dangling. The rules differ per container,
and getting this wrong is undefined behaviour — meaning it might appear to work.

| Container | Insert | Erase |
|---|---|---|
| `std::map` | all iterators stay valid | only the erased one is invalidated |
| `std::deque` | **may invalidate all** | **may invalidate all** |
| `std::unordered_map` | may invalidate on rehash | only the erased one |

`std::map` is generous. `std::deque` is not — never hold a reference or iterator into a deque
across a modification of that deque.

### The classic bug

```cpp
// WRONG — undefined behaviour
for (auto it = asks.begin(); it != asks.end(); ++it) {
    if (it->second.empty()) asks.erase(it);   // it is now dangling; ++it is UB
}
```

Erasing invalidates `it`, and the loop then increments a dead iterator.

`map::erase` returns an iterator to the next element, which is the fix:

```cpp
// RIGHT
for (auto it = asks.begin(); it != asks.end(); ) {
    if (it->second.empty()) it = asks.erase(it);   // advance via the return value
    else                    ++it;
}
```

In your matching loop the same trap appears differently: **finish matching at a level, then
erase it.** Never erase the level you're still working inside.

Run your tests under AddressSanitizer (`-DMMS_SANITIZE=ON`). It catches exactly this.

---

## Part D — `auto`, references, and structured bindings

### `auto`

Tells the compiler to deduce the type.

```cpp
auto it = asks.begin();
// instead of std::map<Price, PriceLevel, std::greater<Price>>::iterator it = ...
```

Use it for iterators and other unspeakable types. Avoid it where the type is the point —
`auto x = book.best_bid();` hides that you're getting a `Price`, and hiding that is not a favour
to the reader.

### `auto&` vs `auto` — this one causes real bugs

```cpp
auto  level = it->second;   // COPY. Modifying it changes nothing.
auto& level = it->second;   // REFERENCE. Modifying it modifies the book.
```

Forgetting the `&` gives you code that compiles, runs, and silently does nothing — you fill
orders in a temporary copy that's discarded at the end of the scope. It is maddening to debug
because the logic *looks* right.

**Habit: when you intend to modify, write `auto&`. When you only read, write `const auto&`.
Write bare `auto` only for iterators and small values.**

### Range-based for

```cpp
for (const auto& order : level.queue) { ... }   // read-only, no copies
for (auto& order : level.queue)       { ... }   // modifiable
```

Cleaner than an index loop when you don't need the index. Same invalidation rules apply — do
not erase from the container you're ranging over.

### Structured bindings (C++17)

```cpp
for (const auto& [price, level] : asks) {
    // price is it->first, level is it->second
}
```

Unpacks a pair or struct into named variables. Much more readable than `.first` / `.second`
scattered through the code. Note the `&` still matters: `auto [p, l]` copies.

---

## Part E — Designing `Impl`

```cpp
namespace mms {

namespace {   // anonymous namespace — internal linkage, invisible outside this file

struct RestingOrder {
    OrderId   id        = 0;
    Quantity  remaining = 0;
    Timestamp timestamp = 0;
    int       owner     = 0;
};

struct PriceLevel {
    std::deque<RestingOrder> queue;
    Quantity total = 0;      // cached sum of queue quantities — keep in sync!

    bool empty() const { return queue.empty(); }
};

struct Locator { Side side; Price price; };

} // anonymous namespace

struct OrderBook::Impl {
    std::map<Price, PriceLevel, std::greater<Price>> bids;  // best = begin()
    std::map<Price, PriceLevel, std::less<Price>>    asks;  // best = begin()
    std::unordered_map<OrderId, Locator>             index;
    std::size_t   live_orders = 0;
    TradeCallback on_trade;
};

} // namespace mms
```

**Anonymous namespace** — `namespace { ... }` with no name gives everything inside *internal
linkage*: visible only within this translation unit. It's the modern replacement for C's
`static` at file scope. Use it for helper types and functions that shouldn't leak into the
linker's symbol table.

**Why `RestingOrder` and not `Order`?** A resting order doesn't need `price` (that's the key of
the level it lives in), `side` (that's which map it's in), or `type` (only limits rest). Storing
them wastes memory and creates two sources of truth that can disagree. Store only what the level
doesn't already know.

**The cached `total`** turns `size_at()` from O(n) into O(1). The cost: every path that changes
the queue must update it. Miss one and your book reports quantities that don't exist. A cached
value is a promise, and every mutation is a chance to break it — worth a test that recomputes
the sum from the queue and asserts it matches.

**`live_orders`** likewise, so `order_count()` is O(1).

### Two invariants to keep in your head

1. `level.total == sum of remaining over level.queue`
2. `index` contains exactly the ids that are live in some level — no more, no less

Almost every bug in this stage is one of these two drifting.

---

## Part F — The template helper

`bids` and `asks` are different types (different comparators), so a normal function can't take
either. A template can:

```cpp
template <typename BookSide>
Quantity match(BookSide& opposite, const Order& incoming, Quantity& remaining);
```

`template <typename T>` means *the compiler generates a version of this function for each type
it's called with*. Call it with `bids` and it makes a bids version; call it with `asks` and it
makes an asks version. Two functions from one piece of source, resolved at compile time, fully
inlinable. No virtual calls.

This is the compile-time polymorphism from the architecture discussion. Write the matching logic
once, get both sides.

Because the definition lives in the same `.cpp` as the only callers, you don't hit the usual
"template definitions must be visible at the point of use" problem.

**Alternative if templates feel like too much at once:** write the buy path and the sell path as
two explicit functions, get the tests green, then refactor to a template. That refactor is a
good commit and a good interview story. Working and duplicated beats elegant and broken.

---

## Part G — Build it in four sub-steps

### 3.1 — Lifecycle and trivial queries

`Impl` definition, constructor, destructor, moves, `set_trade_callback`, `clear`,
`order_count`, `contains`. No matching, no resting yet.

Test: construct a book, assert it's empty, move it, destroy it. Run under ASan.

### 3.2 — Resting and cancelling

`submit` for non-marketable limit orders only (just rest them, no matching), `cancel`,
`best_bid`, `best_ask`, `size_at`, `total_quantity`, `top_of_book`, `depth`.

Test: add orders, check best prices and sizes, cancel from front, middle and back, cancel an
unknown id, cancel the same id twice, cancel the last order at a level and verify the level
disappears.

### 3.3 — Matching

The core loop. Limit orders only.

Test: crossing trades, execution at the resting price, sweeping multiple levels, FIFO within a
level, partial fills keeping queue position.

### 3.4 — Market and IOC, and the invariants

Market orders (always marketable, never rest), IOC (fill then discard). Then the two property
tests: the book never crosses itself, and quantity is conserved.

Commit after each sub-step.

---

## Part H — The bugs you will hit

Ordered by how much time they cost people.

**1. `auto` instead of `auto&`.** You fill orders in a copy. Everything compiles, nothing
changes. Symptom: quantities never decrease.

**2. Erasing a level while iterating it.** Undefined behaviour. May appear to work in Debug and
crash in Release, or vice versa. Symptom: random crashes, corrupted books. ASan catches it.

**3. Forgetting to update `index` on a full fill.** The order is gone from the deque but still
in the index. A later `cancel` on that id finds a locator pointing at a level that no longer
holds it. Symptom: `contains()` returns true for a filled order.

**4. `total` drifting.** Update it on every single path: rest, partial fill, full fill, cancel.
Symptom: `size_at` returns a number that doesn't match the visible orders.

**5. Trading at the wrong price.** Using the aggressor's price rather than the resting order's.
Symptom: a test asserting a trade at 100 sees 105.

**6. Partial fill losing queue position.** If you pop the front and push the remainder to the
back, you've silently reordered the queue. The remainder must stay at the front.

**7. Signed/unsigned mixing.** `-Wconversion` will catch most of it. Don't silence it with a
cast without understanding what's being converted.

---

## Checklist

- [ ] 3.1 lifecycle — commit
- [ ] 3.2 resting and cancel — commit
- [ ] 3.3 matching — commit
- [ ] 3.4 market, IOC, invariants — commit
- [ ] Full run under `-DMMS_SANITIZE=ON` clean
- [ ] Merge `feature/matching-engine` into `develop` with `--no-ff`
- [ ] This is the point where `develop` can go to `main`

---

## Glossary

| Term | Meaning |
|---|---|
| **Iterator** | Generalised pointer; `*it` reads, `++it` advances |
| **`end()`** | One past the last element — never dereference |
| **Iterator invalidation** | Modifying a container can leave iterators dangling |
| **`std::map`** | Ordered tree; O(log n); sorted iteration; `begin()` is smallest |
| **Comparator** | Third template parameter deciding sort order (`std::greater` reverses it) |
| **`std::deque`** | O(1) push/pop at both ends; O(n) erase from the middle |
| **`std::unordered_map`** | Hash table; O(1) average lookup; unordered |
| **`auto`** | Compiler deduces the type |
| **`auto&`** | Deduced type, *by reference* — required to modify in place |
| **Structured binding** | `auto& [k, v] = pair;` — unpack into named variables |
| **Anonymous namespace** | `namespace { }` — internal linkage, file-private |
| **Template** | Compiler generates one version per type used |
| **Invariant** | A condition that must hold before and after every operation |
