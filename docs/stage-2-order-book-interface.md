# Stage 2 — The order book interface

**What you'll write:** `include/mms/order_book.hpp`
**Branch:** `feature/order-book-interface`
**Assumed knowledge:** Stage 1 only. Every new C++ construct is explained from zero.

This stage contains no algorithms. You are designing a **contract**: what the order book
promises to do, without saying how. The implementation comes in Stage 3.

Getting this right matters more than it looks. The interface determines what's possible
later — including whether the P&L attribution in Phase 4 can be computed at all.

---

## Part A — What an interface file is for

A header answers: *what can I do with this thing?* Not *how does it work?*

Two independent reasons this separation matters:

**Compilation cost.** Every `.cpp` that includes your header gets a literal copy pasted in
(Stage 1, Part A). If the header contains implementation, every change to that implementation
forces a rebuild of every file that includes it. On a large codebase this is the difference
between a two-second and a two-minute edit cycle.

**Coupling.** If your header says `std::map<Price, PriceLevel> bids_;`, then `std::map` is now
part of your public API. Every user of your class knows you use a map. Change to a vector and
you've broken an interface promise you didn't intend to make.

Your goal: a reader should understand the entire order book from this file, and learn nothing
about how it's stored.

---

## Part B — `class` vs `struct`, and access control

In C, `struct` is a bag of fields. In C++, `struct` and `class` are **the same construct** with
one difference: default access.

```cpp
struct S { int x; };   // x is PUBLIC by default
class  C { int x; };   // x is PRIVATE by default
```

That's the entire difference. Not "structs are data, classes have methods" — a struct can have
methods and a class can be pure data.

**Three access levels:**

- **`public`** — anyone can access
- **`private`** — only members of this class
- **`protected`** — this class and derived classes (irrelevant here; you have no inheritance)

**The convention, which you should follow:** use `struct` for plain data with no invariants
(your `Order`, `Trade`, `TopOfBook` — every field is independently valid), and `class` when the
object maintains an **invariant** that must never be violated.

`OrderBook` has invariants:

- The book must never cross (`best_bid < best_ask`)
- Cached quantities must equal the sum of the orders at that level
- The id→location map must agree with the actual contents

If those fields were public, any caller could break them. Making them private means the only
way to modify the book is through methods you wrote, and those methods can guarantee the
invariant still holds when they return. **That is what encapsulation is actually for** — not
hiding for its own sake, but making illegal states unreachable.

Order your class `public:` first. Readers care about the interface; the private section is
implementation detail and belongs at the bottom.

**Naming convention:** private data members get a trailing underscore — `impl_`, `bids_`. It
makes `x_ = x;` in a constructor unambiguous and instantly signals scope when reading a method
body. Adopt it and be consistent.

---

## Part C — Constructors and destructors

A **constructor** runs when an object is created. A **destructor** runs when it's destroyed.

```cpp
class OrderBook {
public:
    OrderBook();     // constructor — no return type, name matches class
    ~OrderBook();    // destructor — tilde, no return type, no parameters
};
```

The destructor's guarantee is the foundation of everything else in C++: **it runs
automatically** when the object goes out of scope, whether by normal exit, early `return`, or
an exception unwinding the stack.

```cpp
void f() {
    OrderBook book;      // constructor runs
    if (something) return;   // destructor runs here
    // ...
}                        // or here
```

This is **RAII** — Resource Acquisition Is Initialisation, the worst-named good idea in
programming. What it means: tie a resource's lifetime to an object's lifetime, and cleanup
becomes automatic and impossible to forget.

In C you wrote `malloc` and had to reach every `free` on every path, including error paths.
That's where leaks come from. In C++ the destructor is the `free`, and the compiler emits the
call on every path for you.

---

## Part D — Copy and move semantics

This is the part you'll have forgotten, and it's the most important C++ concept in this file.
Take it slowly.

### The problem

By default, C++ gives every class a **copy constructor** and **copy assignment operator** that
copy each member. For an `int` that's fine. For a class holding a pointer, it's a disaster:

```cpp
class Bad {
    int* data_;
public:
    Bad()  { data_ = new int[1000]; }
    ~Bad() { delete[] data_; }
};

Bad a;
Bad b = a;    // default copy: b.data_ = a.data_  — SAME pointer
```

Now both objects hold the same pointer. When they go out of scope, both destructors run
`delete[]` on it. **Double free.** Immediate crash, or silent heap corruption if you're
unlucky, which is worse.

This is the classic C++ trap and the reason the next concepts exist.

### Value categories: lvalues and rvalues

An **lvalue** is something with a name and an address — something you could take the address of.
An **rvalue** is a temporary, with no name, about to be destroyed.

```cpp
int x = 5;
x        // lvalue — has a name, persists
5        // rvalue — a temporary
x + 1    // rvalue — the result is temporary
f()      // rvalue — the returned temporary
```

The insight that produced move semantics: **if a value is about to be destroyed anyway, you
don't need to copy from it — you can steal from it.**

Copying a 10,000-element vector allocates new memory and copies 10,000 elements. But if the
source is a temporary that's about to die, you can just take its pointer and leave it empty.
O(n) becomes O(1).

### `&` and `&&`

```cpp
void f(OrderBook&  b);   // lvalue reference — binds to named objects
void f(OrderBook&& b);   // rvalue reference — binds to temporaries
```

`&&` is not "and". In a type it means **rvalue reference**: a reference that only binds to
temporaries. Its purpose is to let the compiler pick a different overload when it knows the
source is expendable.

### Move constructor and move assignment

```cpp
OrderBook(OrderBook&& other) noexcept;             // move constructor
OrderBook& operator=(OrderBook&& other) noexcept;  // move assignment
```

A move constructor **transfers ownership** rather than duplicating:

```cpp
OrderBook::OrderBook(OrderBook&& other) noexcept
    : impl_(other.impl_)     // take their pointer
{
    other.impl_ = nullptr;   // and leave them empty — CRITICAL
}
```

The `other.impl_ = nullptr;` is not optional. Without it both objects hold the pointer and
you're back to double-free. The rule: **after a move, the source must be left in a valid,
destructible state.** Empty is valid. Dangling is not.

Move assignment additionally has to handle the object already owning something, plus
self-assignment:

```cpp
OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    if (this != &other) {        // guard against b = std::move(b)
        delete impl_;            // release what we already hold
        impl_ = other.impl_;     // steal
        other.impl_ = nullptr;   // clear the source
    }
    return *this;                // enables chaining: a = b = c
}
```

`this` is a pointer to the current object, available inside any non-static member function.
`*this` dereferences it. Returning `OrderBook&` — a reference, not a copy — is what makes
`a = b = c` work.

**Why `noexcept` on moves matters concretely:** `std::vector` reallocates when it grows. If your
type's move constructor is `noexcept`, vector *moves* the elements. If it isn't, vector must
*copy* them — because if a move threw halfway through, the vector would be left with some
elements moved and some not, and it couldn't recover. So a missing `noexcept` silently turns
O(1) moves into O(n) copies. Always mark moves `noexcept`.

### `= delete`

```cpp
OrderBook(const OrderBook&)            = delete;
OrderBook& operator=(const OrderBook&) = delete;
```

This tells the compiler: *this function exists, and using it is an error.* Any attempt to copy
an `OrderBook` now fails at compile time with a clear message.

Why forbid copying a book? Two reasons, one practical and one about intent:

- The book will own heap-allocated state. A correct copy means a deep copy, which is work to
  write and easy to get wrong.
- An order book is a **unique thing**. It represents one venue's state. Two identical copies is
  not a meaningful concept. Copying is almost certainly a bug — usually an accidental
  pass-by-value in a function signature.

Deleting it turns that silent, expensive bug into a compile error.

### The Rule of Five and the Rule of Zero

**Rule of Five:** if you define any of these, you probably need all five —

1. Destructor
2. Copy constructor
3. Copy assignment
4. Move constructor
5. Move assignment

Because needing a custom destructor means you're managing a resource, and every one of the
other four has to handle that resource correctly too.

**Rule of Zero:** better still, define *none* of them. Use members that manage themselves
(`std::vector`, `std::unique_ptr`), and the compiler-generated versions are all correct.

You are deliberately writing the Rule of Five version here, using a raw pointer, because doing
it once teaches you what `unique_ptr` does for you. Note in `DESIGN.md` that `std::unique_ptr`
would let you delete four of these five declarations — knowing that is the point.

---

## Part E — The pimpl idiom

**Pimpl** = "pointer to implementation". You already know this from C: forward-declare a struct
and hold a pointer to it, so callers never see its contents.

```cpp
class OrderBook {
public:
    // ... methods ...
private:
    struct Impl;    // declared, not defined — an "incomplete type"
    Impl* impl_;
};
```

`struct Impl;` declares that a type by this name exists, without saying what's in it. That's an
**incomplete type**. You can hold a pointer to an incomplete type (all pointers are the same
size, so the compiler knows enough), but you cannot create one or access its members — the
compiler doesn't know its size or layout.

`Impl` is then fully defined in `order_book.cpp`, where the compiler can see it.

**What this buys you:**

- Your header includes no `<map>`, `<deque>`, `<unordered_map>` — so files including you don't
  pay to compile them either
- Changing the internal data structure recompiles exactly one `.cpp`, not everything downstream
- Your container choice (Part H) stays genuinely private

**What it costs:** one extra pointer dereference per call, and a heap allocation per book.
Irrelevant here — books are constructed once, not in a loop. Write that trade-off in `DESIGN.md`.

**The trap.** The destructor **must** be declared in the header and *defined in the `.cpp`*.
If you let the compiler generate it, it generates it wherever the class is used — where `Impl`
is still incomplete — and `delete impl_;` on an incomplete type is undefined behaviour. GCC
warns; some compilers silently do the wrong thing.

So: `~OrderBook();` in the header, `OrderBook::~OrderBook() { delete impl_; }` in the `.cpp`.
Same for the move operations.

---

## Part F — `std::function` and the callback

```cpp
#include <functional>

using TradeCallback = std::function<void(const Trade&)>;
```

`std::function<void(const Trade&)>` means *any callable that takes a `const Trade&` and returns
`void`.* It can hold a free function, a lambda, a lambda that captures state — anything callable
with that signature.

In C you had function pointers, which can't carry state. `std::function` can, which is what
makes this work:

```cpp
std::vector<Trade> tape;
book.set_trade_callback([&tape](const Trade& t) { tape.push_back(t); });
```

`[&tape](const Trade& t){ ... }` is a **lambda** — an anonymous function. The `[&tape]` is the
**capture list**: `&tape` means capture by reference, so the lambda can push into the caller's
vector. A plain function pointer could not do this.

**Capture by reference (`&`) vs by value (`=`):** `[&x]` refers to the caller's `x`, so the
lambda must not outlive it. `[x]` copies. Dangling reference captures are a real source of bugs;
prefer capturing what you actually need, by name, rather than `[&]`.

### Why a callback rather than returning a list of trades

`submit()` could return `std::vector<Trade>`. A callback is better here:

- **No allocation per call.** Returning a vector allocates on every submit, even when nothing
  trades — which is most of the time. `submit()` is your hot path.
- **Streaming.** The consumer sees each trade as it happens, in order.
- **Decoupling.** The book doesn't know or care whether the consumer is a test tape, a P&L
  engine, or a CSV writer. That's the dependency rule from the architecture discussion:
  the core knows nothing about the outside.

**Honest cost:** `std::function` is a type-erased indirect call — it can't be inlined, roughly
what a virtual call costs. Once per *trade*, not once per instruction, so it almost certainly
doesn't matter. Record it in `DESIGN.md` as a known cost with a known fix (template the sink on
the callback type). "I knew, I measured, it wasn't the bottleneck" is a strong interview answer.
"I never considered it" is not.

---

## Part G — The interface, method by method

Here is the file. Transcribe it, but for each method ask yourself *what does this promise?*
before writing it.

```cpp
#pragma once

#include "mms/types.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace mms {

class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    OrderBook();
    ~OrderBook();

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept;
    OrderBook& operator=(OrderBook&&) noexcept;

    void set_trade_callback(TradeCallback cb);

    // --- order entry ---
    Quantity submit(const Order& order);
    bool     cancel(OrderId id);

    // --- queries ---
    TopOfBook   top_of_book() const;
    Price       best_bid() const;
    Price       best_ask() const;
    Quantity    size_at(Side side, Price price) const;
    Quantity    total_quantity(Side side) const;
    std::size_t order_count() const;
    bool        contains(OrderId id) const;

    struct Level {
        Price       price;
        Quantity    quantity;
        std::size_t order_count;
    };
    std::vector<Level> depth(Side side, std::size_t levels) const;

    void clear();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace mms
```

### `Quantity submit(const Order& order)`

**`const Order&` — pass by const reference.** Not by value.

- **Reference (`&`)** avoids copying the struct. `Order` is ~48 bytes; copying it per call is
  waste in your hottest function.
- **`const`** promises you won't modify the caller's object, and lets the function accept
  temporaries.

The general rule: pass by `const&` for anything bigger than a couple of machine words; pass by
value for `int`-sized things, where a copy is cheaper than an indirection.

**Returns `Quantity`** — how much executed immediately. `0` means it rested without trading.
The caller usually needs this to know whether their order is live.

Note the design choice: the return value tells you *how much*, the callback tells you *at what
prices*. Deliberate — most callers only need the total, and only P&L needs the detail.

### `bool cancel(OrderId id)`

Returns `true` if an order was found and removed.

**It must not throw on an unknown id.** This is a domain fact, not a style preference: in a real
system, cancels race against fills constantly. You send a cancel; the order fills a microsecond
before it arrives. Your cancel now targets an order that no longer exists. That is **normal
operation**, not an error. An exception here would mean an exception on a routine event.

Returning `false` says "nothing to cancel" and the caller decides whether it cares.

### The `const` query methods

Every one of these is marked `const`: they answer questions without changing the book. The
compiler enforces it, and it means a `const OrderBook&` is still fully queryable.

`best_bid()` and `best_ask()` return `kInvalidPrice` when that side is empty — the sentinel from
Stage 1.

`size_at(side, price)` returns 0 for an empty level. Note it takes both a side and a price
rather than searching both sides: a price could theoretically exist on either side, and forcing
the caller to say which removes the ambiguity.

`order_count()` is the number of live **orders**, not price levels. Useful for tests: after
cancelling everything it must be zero.

### `struct Level` and `depth()`

A **nested type** — `Level` is scoped inside `OrderBook`, so outside it's `OrderBook::Level`.
Nest it because it has no meaning independent of the book.

```cpp
std::vector<Level> depth(Side side, std::size_t levels) const;
```

Returns the top N price levels, best first, aggregated per price.

**Returning a `vector` by value is correct here and does not copy.** The compiler either elides
the copy entirely (copy elision, guaranteed in C++17 for the returned temporary) or moves it.
This is a place where the old C habit of "return through an out-parameter to avoid copying"
is obsolete — write the clear signature.

`std::size_t` is the standard unsigned type for sizes and counts, from `<cstddef>`. Use it for
anything counting elements; it's what `.size()` returns everywhere in the standard library.

### Include hygiene

Include what you use, and nothing more:

- `"mms/types.hpp"` — you use `Price`, `Order`, `Trade`
- `<functional>` — `std::function`
- `<vector>` — the return type of `depth`
- `<cstddef>` — `std::size_t`

No `<map>`, no `<deque>`. Those belong to the implementation, and the pimpl is what keeps them
out. If you find yourself needing a container header here, the pimpl has leaked.

---

## Part H — D2: the container decision

This is the interview centrepiece. Not because the answer is hard, but because **how you choose**
is exactly what a market-making firm is testing for.

You must store, per side, a set of price levels, each holding a FIFO queue of orders. Three
realistic options:

### (a) `std::map<Price, PriceLevel>`

An ordered tree. Sorted by construction, so the best price is `begin()`.

- **Good:** simple, correct, handles any price range, best price in O(1) via `begin()`
- **Bad:** O(log n) insert and lookup; **one heap allocation per price level**; nodes scattered
  across memory, so walking levels is a series of cache misses

For bids you want descending order, which is `std::map<Price, PriceLevel, std::greater<Price>>`.
Then `begin()` is the best price on *both* sides, and your matching loop needs no branch on side.

### (b) `std::vector<PriceLevel>` indexed by `price - base_price`

A flat array covering a price band.

- **Good:** O(1) lookup by price, contiguous memory, excellent cache behaviour. This is what
  most production books do.
- **Bad:** memory proportional to the band, not to the number of live levels. Needs a policy for
  prices outside the band, and finding the best price means scanning from a cached hint.

The justification is empirical: real instruments trade in a narrow band intraday. Reserving
±1000 ticks costs a few tens of KB and covers essentially every price you'll see.

### (c) Intrusive linked list of levels + hash map for lookup

O(1) both ways, more code, more pointer chasing.

### How to decide

Answer these, in writing:

1. What price range will your simulation actually use? (You control the flow generator — you
   know this.)
2. Which operation is most frequent? (Hint: in real markets, cancels vastly outnumber fills.)
3. Are you optimising for correctness-first or throughput-first *at this stage*?

**A defensible recommendation:** start with (a) `std::map`. It's harder to get wrong, and Stage 3
is about matching logic, not memory layout. Then in Phase 5 you benchmark, discover the
allocation cost, and switch to (b) — and you'll have *measured evidence* for the change.

That story — "I started simple, benchmarked, found the bottleneck, changed it, measured again" —
is worth far more in an interview than having guessed correctly on day one. It demonstrates the
thing they're actually screening for: that you make decisions from data rather than from taste.

### D3: cancels from the middle of a queue

Sitting behind D2 is a subtler problem. Orders in a price level are FIFO. Cancels can target
**any** order in the queue, not just the front. And cancels are the most common message type in
real markets.

- `std::deque` with immediate erase: O(1) at the ends, **O(n) from the middle**
- **Lazy deletion:** mark the order dead, skip it while matching, sweep periodically. O(1)
  cancel, at the cost of memory held by dead orders and a slightly more complex match loop.
- **Intrusive doubly-linked list** + `unordered_map<OrderId, node*>`: O(1) cancel *and* O(1)
  removal. One allocation per order.

You don't have to solve this now. You have to **record that you know it's there**. Write D3 in
`DESIGN.md` with the options and note that you'll decide after benchmarking.

An interviewer who asks "what happens when I cancel an order in the middle of the queue?" and
hears "that's O(n) with a deque, I noted it as a known cost and planned to fix it after
measuring" gets a much better signal than one who hears silence.

---

## Checklist

- [ ] Write `include/mms/order_book.hpp`
- [ ] Confirm it compiles standalone:
      `g++ -std=c++17 -Iinclude -fsyntax-only` on a file that includes only this header
- [ ] Confirm the header includes no container headers
- [ ] `DESIGN.md` **D2** — container choice: Decision / Alternatives / Why / Cost / What would change it
- [ ] `DESIGN.md` **D3** — cancel from mid-queue: state the problem and the options, even if undecided
- [ ] `DESIGN.md` **D4** — pimpl: why, and what it costs
- [ ] Commit, merge into `develop` with `--no-ff`

---

## Glossary

| Term | Meaning |
|---|---|
| **Encapsulation** | Making invariants unbreakable by hiding the data behind methods |
| **RAII** | Resource lifetime tied to object lifetime; destructor does the cleanup |
| **lvalue** | Has a name and an address; persists |
| **rvalue** | A temporary, about to be destroyed |
| **`&&`** (in a type) | Rvalue reference — binds only to temporaries |
| **Move** | Transfer ownership instead of copying; source left empty but valid |
| **Rule of Five** | Define one of dtor/copy/move → probably need all five |
| **Rule of Zero** | Better: use self-managing members, define none of them |
| **`= delete`** | This function exists and using it is a compile error |
| **Pimpl** | Pointer to an incomplete `Impl` type, defined only in the `.cpp` |
| **Incomplete type** | Declared but not defined; you may hold a pointer, nothing more |
| **`std::function`** | Type-erased callable — a function pointer that can carry state |
| **Lambda** | Anonymous function, `[captures](params){ body }` |
| **Capture list** | What a lambda borrows (`&x`) or copies (`x`) from its scope |
| **Copy elision** | Compiler constructs the return value in place; no copy at all |
| **`std::size_t`** | Standard unsigned type for sizes and counts |
