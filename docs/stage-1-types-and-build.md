# Stage 1 — Domain types and the build system

**What you wrote:** `include/mms/types.hpp`, `CMakeLists.txt`
**Assumed knowledge:** none. Every C++ construct in your file is explained from zero.

---

## Part A — How C++ code becomes a program

You cannot debug build errors without this model, so it comes first.

Turning source into a running program takes four steps. Each has its own kind of error, and knowing which step failed tells you where to look.

### 1. Preprocessing

Before the compiler sees anything, the **preprocessor** does dumb text substitution. Every line starting with `#` is an instruction to it.

`#include <cstdint>` means *find the file `cstdint` and paste its entire contents here.* Literally paste. After preprocessing, your 75-line `types.hpp` is thousands of lines.

That is all `#include` does. It is not an import statement like Python's. There are no modules, no namespaces involved, no symbol resolution — just copy and paste.

`#include <angle brackets>` searches the system and configured include directories.
`#include "quotes"` searches relative to the current file first, then falls back to the same places.

### 2. Compilation

The compiler takes each `.cpp` file — after preprocessing — and turns it into an **object file** (`.o`), containing machine code.

The critical thing: **each `.cpp` is compiled completely independently.** The compiler working on `types.cpp` knows nothing about `order_book.cpp`. It has never seen it.

That single fact explains the entire header/source split, which we'll get to.

A unit of work here — one `.cpp` plus everything it includes — is called a **translation unit**.

### 3. Linking

The **linker** takes all the `.o` files and stitches them into one library or executable. Its job is resolving cross-references: `order_book.o` calls a function that lives in `types.o`, and the linker connects them.

### 4. Running

Errors are diagnosable by *which step failed*:

| Error says | Failed at | Meaning |
|---|---|---|
| `No such file or directory` | preprocessing | An `#include` path is wrong |
| `expected ';'`, `'X' was not declared` | compilation | Syntax or a missing declaration |
| `undefined reference to 'f'` | **linking** | You *declared* `f` but never *defined* it, or forgot to add its `.cpp` to the build |
| Crash, wrong output | runtime | Logic bug |

`undefined reference` is the one that confuses people most. It means the compiler was happy — it believed your promise that the function existed somewhere — and the linker then couldn't find it. Almost always: you forgot to list a `.cpp` in `CMakeLists.txt`.

---

## Part B — Declarations vs definitions, and why headers exist

A **declaration** says *this thing exists somewhere, here's its shape*:

```cpp
int add(int a, int b);        // declaration — no body
```

A **definition** provides the actual thing:

```cpp
int add(int a, int b) { return a + b; }   // definition — has a body
```

The compiler only needs a **declaration** to compile code that *uses* something. The linker needs the **definition** to exist somewhere across the whole program.

This produces the two rules that govern all C++ file layout:

- **A declaration may appear many times** (in every file that uses the thing).
- **A definition may appear exactly once** in the whole program. This is the *One Definition Rule*, or ODR.

Now the header/source split makes sense:

- **Header (`.hpp`)** — declarations. Included by many `.cpp` files. Says *what exists*.
- **Source (`.cpp`)** — definitions. Compiled once. Says *how it works*.

### `#pragma once`

Because `#include` is literal copy-paste, a header can end up pasted twice into the same translation unit — say `A.hpp` includes `types.hpp`, and your `.cpp` includes both `A.hpp` and `types.hpp`. Now every struct is defined twice: ODR violation, compile error.

`#pragma once` at the top tells the preprocessor *if you've already pasted this file into this translation unit, skip it.*

The older, more portable form you saw at 42 does the same thing manually:

```cpp
#ifndef MMS_TYPES_HPP
#define MMS_TYPES_HPP
...
#endif
```

`#pragma once` is one line instead of three, cannot break through a typo'd macro name, and every compiler you will realistically meet supports it. Use it.

### Why your `types.hpp` needs no `.cpp`

Your header contains only **types** (structs, enums, aliases) and **`constexpr` functions**. None of those generate code that needs to exist once — they're descriptions the compiler uses at each use site.

`src/types.cpp` will only be needed later, when you add functions with real bodies like printing helpers.

---

## Part C — Every construct in your file

### `namespace mms { ... }`

A namespace is a named scope that prevents collisions. Your `Order` is really `mms::Order`, so it can coexist with any other library's `Order`.

Inside the namespace you write `Price`. Outside, you write `mms::Price`, or bring names in with `using namespace mms;`.

**Never put `using namespace` in a header.** Everyone who includes your header would silently inherit it — you'd be dumping your names into their scope without their consent. In a `.cpp`, it's fine.

### `using Price = std::int64_t;`

A **type alias**: a new name for an existing type. `Price` *is* `std::int64_t` — not a wrapper, not a distinct type. Zero runtime cost; the compiler substitutes it.

The C way was `typedef std::int64_t Price;`. `using` reads left-to-right like an assignment and works with templates, which `typedef` doesn't. Prefer `using`.

**Important limitation, and it's a design point:** because these are aliases and not distinct types, the compiler will happily let you do this:

```cpp
Price p = 100;
Quantity q = 50;
p = q;              // compiles fine — both are int64_t
```

That's a real bug the type system doesn't catch. The fix is "strong typedefs" — wrapping each in its own struct so they become genuinely different types. It costs boilerplate and buys compile-time safety.

You don't need to do it. You *do* need to know it's a known limitation of your design. That's a `DESIGN.md` entry and a good interview answer.

### Fixed-width integers

```cpp
std::int64_t    // signed, exactly 64 bits
std::uint64_t   // unsigned, exactly 64 bits
std::uint8_t    // unsigned, exactly 8 bits (one byte)
```

From `<cstdint>`. Use these instead of `int`, `long`, `short`.

**Why.** The built-in types have *implementation-defined* sizes. `int` is usually 32 bits. `long` is 64 bits on Linux and macOS but **32 bits on Windows** — code that assumes otherwise breaks when ported, usually silently, usually via overflow.

`std::int64_t` means 64 bits everywhere. No ambiguity.

**Signed vs unsigned, and the trap.** Unsigned types cannot represent negatives. They *wrap around*:

```cpp
unsigned int a = 3, b = 5;
a - b;    // NOT -2. It is 4294967294.
```

Worse, mixing signed and unsigned in one expression makes C++ silently convert the signed value to unsigned, which reintroduces the bug in code that looks correct.

This is why `Quantity` is **signed** even though a quantity is never negative: you subtract quantities constantly, and inventory genuinely goes negative when you're short. Keeping everything signed makes the bug unwriteable.

`OrderId` is unsigned because it's a *label*, never arithmetic. That's a deliberate signal to the reader.

### `inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();`

Three separate ideas. Take them one at a time.

**`std::numeric_limits<Price>::min()`** — from `<limits>`, gives the smallest value the type can hold. For `int64_t` that's about −9.2 × 10¹⁸. The `<Price>` part is a *template argument*: `numeric_limits` is a template that works for any numeric type, and you're asking for the `Price` version.

**`constexpr`** — "computable at compile time." The compiler evaluates it during compilation and bakes the result into the binary. No runtime work, and the value can be used where a compile-time constant is required (array sizes, template arguments, `switch` labels).

`const` and `constexpr` are different:

```cpp
const int a = f();       // a never changes, but f() runs at runtime
constexpr int b = 42;    // known at compile time
```

`constexpr` implies `const`. The reverse isn't true.

**`inline`** — this one has a misleading name. In modern C++ `inline` does **not** mean "inline this function for speed." It means *this definition may legally appear in multiple translation units; linker, please merge them into one.*

Without `inline`, every `.cpp` including your header would define its own `kInvalidPrice`, and the linker would report a duplicate symbol. `inline constexpr` at namespace scope in a header is the standard C++17 idiom for a header-only constant.

**Why `min()` as the sentinel?** You need a value meaning "no price exists." `min()` is the most extreme negative number, so if a bug ever leaks it into price comparisons it sorts to one end and produces obviously wrong output rather than plausibly wrong output. **Fail loudly, not quietly.**

The alternative is `std::optional<Price>`, which is more correct — it makes "no price" a distinct state the type system enforces — at the cost of unwrapping at every use. Note the trade-off in `DESIGN.md`.

### `enum class Side : std::uint8_t { Buy = 0, Sell = 1 };`

An enum is a type with a fixed set of named values.

**`enum class` vs plain `enum` — this is the bug in your file, so read carefully.**

Old-style C enum:

```cpp
enum Side { Buy, Sell };
Side s = Buy;              // unqualified — Buy leaks into the enclosing scope
int x = s;                 // implicit conversion to int — allowed
if (s == 1) { }            // compiles. Is 1 Buy or Sell? Who knows.
```

Two problems: the names escape into the surrounding scope, so a different enum with a `Buy` collides; and it converts to `int` implicitly, so nonsense comparisons compile.

Scoped enum (C++11):

```cpp
enum class Side { Buy, Sell };
Side s = Side::Buy;        // must qualify — no scope pollution
int x = s;                 // COMPILE ERROR — no implicit conversion
if (s == 1) { }            // COMPILE ERROR
if (s == Side::Buy) { }    // correct
```

Now the compiler catches a whole category of mistake for you. **`class` here is lowercase and is a keyword — it is not a type name and it is not capitalised.**

**`: std::uint8_t`** sets the *underlying type* — the integer the enum is stored as. Default is `int` (4 bytes); this makes it 1 byte. Irrelevant for a single value, meaningful when you have millions of orders in memory and care about cache lines.

**`{ Buy = 0, Sell = 1 }`** — explicit values. Enumerators start at 0 and increment by default, so this is what you'd get anyway. Writing it explicitly documents that the values are relied upon.

### `constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }`

**The ternary operator** `condition ? a : b` is an *expression* version of if/else. It evaluates to `a` when the condition is true, `b` otherwise. Same as:

```cpp
if (s == Side::Buy) return Side::Sell;
else                return Side::Buy;
```

It's used here because it's an expression, so it works in a `constexpr` context and in a single `return`.

**`noexcept`** promises this function never throws an exception. Two benefits: the compiler skips generating exception-unwinding machinery, and it documents intent. If a `noexcept` function *does* throw, the program calls `std::terminate` immediately — so only mark functions that genuinely cannot throw. This one does arithmetic on an enum; nothing can throw.

**`constexpr` on a function** means it *may* be evaluated at compile time when its arguments are compile-time constants. If called with a runtime value, it runs normally at runtime. Free optimisation, no downside.

### `sign_of` — small function, large payoff

```cpp
constexpr int sign_of(Side s) noexcept { return s == Side::Buy ? 1 : -1; }
```

This exists so one formula covers both sides:

```cpp
pnl += sign_of(side) * quantity * (exit_price - entry_price);
```

Buy → `+1`, so a price rise is a profit. Sell → `−1`, so a price rise is a loss. Correct in both cases with no branch.

Without it you'd write the buy path and the sell path separately, and one day you'd fix a bug in one and forget the other. **Every branch you eliminate is a bug you cannot write.**

### `struct Order { ... }` and default member initialisers

```cpp
struct Order {
    OrderId  id       = 0;
    Quantity quantity = 0;
};
```

A `struct` groups related data. In C++ (unlike C) a struct can also have member functions — `struct` and `class` are the same thing, differing only in default access: `struct` members are public, `class` members are private.

The `= 0` are **default member initialisers** (C++11). Any `Order` you create without specifying a field gets that default.

This matters more than it looks. In C, this leaves everything as garbage:

```c
struct Order o;   // fields contain whatever was in that memory
```

Uninitialised memory bugs are *non-deterministic* — the program works on your machine and fails in the test suite, because the garbage differed. With default initialisers, an `Order` is always in a defined state. Free correctness; take it.

### Member functions and `const`

```cpp
bool has_bid() const noexcept { return bid_price != kInvalidPrice; }
```

A function inside a struct, operating on that struct's data.

**`const` after the parameter list** — this is the important part. It promises *this function does not modify the object.* The compiler enforces it: try to assign to a member inside a `const` function and it won't compile.

It also determines what you can call. Given a `const TopOfBook&`, you may only call its `const` member functions. If `has_bid()` weren't marked `const`, half your code would fail to compile the moment you passed a book around by const reference.

**Rule: mark every member function `const` that doesn't modify state.** It costs nothing and is painful to retrofit.

**Defining the body inside the struct** — for a one-liner in a header, this is right. The definition is implicitly `inline` (so no ODR violation across translation units), and the compiler can see the body at every call site and inline it away.

Long functions go in the `.cpp`. Short accessors go in the header.

### `mid_x2()` — the half-tick problem

```cpp
Price mid_x2() const noexcept {
    return is_two_sided() ? (bid_price + ask_price) : kInvalidPrice;
}
```

The mid price is `(bid + ask) / 2`. With integer prices this is a trap.

Bid 100, ask 101. True mid is 100.5. Integer division truncates: `201 / 2 == 100`. You've silently lost half a tick.

Half a tick sounds negligible. A market maker earns *fractions of a tick* per round trip. Half a tick of error per fill, accumulated over a simulation, is larger than the entire P&L you're trying to measure. It would not look like a bug — it would look like a strategy that doesn't work.

The fix: return `bid + ask`, twice the mid, and stay in half-tick units through the whole calculation. Divide by two exactly once, at display.

**General rule: never round in the middle of an accumulation.** Carry full precision to the end.

---

## Part D — What CMake actually does

CMake is not a build system. It is a **build system generator**: it reads `CMakeLists.txt` and produces the real build files for your platform (Makefiles on Linux/macOS, Visual Studio projects on Windows, Ninja files if asked). You describe *what* to build; CMake works out *how*.

```cmake
cmake_minimum_required(VERSION 3.16)
project(market_maker_simulator LANGUAGES CXX)
```

Minimum version, then project name and languages. `CXX` means C++.

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

C++17. `REQUIRED ON` means fail rather than silently fall back to an older standard. `EXTENSIONS OFF` means standard C++ (`-std=c++17`) rather than GNU extensions (`-std=gnu++17`) — you want code that compiles anywhere, not just under GCC.

```cmake
add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
```

The flags, individually:

- **`-Wall`** — common warnings. Misleadingly named; it is not all warnings.
- **`-Wextra`** — more warnings that `-Wall` omits.
- **`-Wpedantic`** — warn on non-standard constructs.
- **`-Werror`** — **treat every warning as an error.** Uncomfortable and correct. A warning is the compiler saying "this is legal but probably not what you meant." In a matching engine, "probably not what you meant" is a bug you haven't found. Forcing yourself to fix them keeps the build permanently clean, which means a *new* warning is visible instead of being lost in a wall of old ones.
- **`-Wshadow`** — warn when an inner variable hides an outer one of the same name. A classic source of "why isn't my variable updating."
- **`-Wconversion`** — warn on implicit conversions that may lose data, e.g. assigning `int64_t` into `int`. This is precisely how quantity and price bugs enter a trading system. Noisy at first; keep it.

```cmake
option(MMS_SANITIZE "Build with sanitizers" OFF)
if(MMS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
```

**Sanitizers** instrument your binary to catch bugs at runtime that the compiler cannot see statically.

- **AddressSanitizer** catches use-after-free, buffer overruns, memory leaks.
- **UndefinedBehaviorSanitizer** catches signed overflow, invalid casts, null dereference.

Roughly 2× slower, hence opt-in via `option()`. You will be managing order queues with iterators and pointers — ASan will find bugs that would otherwise cost you an evening each. Run your tests under it regularly.

`-fno-omit-frame-pointer` and `-g` make the stack traces readable.

```cmake
add_library(mms src/types.cpp src/order_book.cpp)
target_include_directories(mms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

`add_library` defines a build target named `mms` from those source files. **Only `.cpp` files are listed — never headers.** Headers aren't compiled independently; they're pasted into `.cpp` files by the preprocessor.

`target_include_directories` adds `include/` to the header search path, which is what makes `#include "mms/types.hpp"` resolve.

**`PUBLIC`** is about propagation. It means: this include path applies to `mms` itself *and* to anything that links against `mms`. So your test executable inherits it automatically. `PRIVATE` would mean "only for building `mms`."

Build and run:

```bash
cmake -B build                 # generate build files into build/
cmake --build build -j         # compile, -j = parallel
```

---

## Part E — Review of your code

Four issues. Two are typos, two are real.

### 1. `enum Class OrderType` — line 26 — **compile error**

```cpp
enum Class OrderType : std::uint8_t {   // wrong
enum class OrderType : std::uint8_t {   // right
```

`class` is a **keyword**, lowercase. `Class` with a capital C is parsed as a type name, so the compiler reads this as "an enum named `Class`" and everything after it collapses. This one error produces eight cascading messages — a good lesson in reading compiler output: **always fix the first error and recompile.** The rest are usually consequences.

### 2. `Price price  0;` — line 44 — **compile error**

Missing `=`. Should be `Price price = 0;`.

### 3. `Side agressor_side` — line 48 — **compiles, and that's the problem**

Missing a `g`. It compiles fine, and then every piece of code you write later referring to `aggressor_side` fails with a confusing "no member named" error. Fix it now, while you know why.

Also line 29: `discart` → `discard` (comment only).

### 4. `../src/order_book.cpp` — **filename typo, will fail at build**

The file is named `oder_book.cpp`. Your `CMakeLists.txt` line 20 references `src/order_book.cpp`. CMake will fail with "Cannot find source file."

```bash
git mv src/oder_book.cpp src/order_book.cpp   # or plain mv, no git history yet
```

### What you got right

Genuinely everything else, including the parts that are easy to get subtly wrong:

- Integer prices with the tick comment
- Correct signed/unsigned choices throughout
- `inline constexpr` for the header constant — the C++17 idiom, and a common mistake to omit `inline`
- `const noexcept` on every accessor in `TopOfBook`
- Default member initialisers on every field of every struct
- `mid_x2` returning the doubled value rather than dividing

The two compile errors are typing, not misunderstanding. Nothing here suggests you've misunderstood the design.

### One cosmetic thing

Lines 7–10 are indented inside the namespace, lines 13 onward aren't. Pick one and be consistent — the common C++ convention is **not** to indent namespace contents, since namespaces often wrap entire files and you'd lose a level of indentation for nothing.

---

## Checklist before Stage 2

- [ ] Fix the four issues above
- [ ] Verify it compiles: `g++ -std=c++17 -Iinclude -fsyntax-only` on a file that includes the header
- [ ] Write `docs/DESIGN.md` entry **D1 — Prices are integers in ticks**: Decision / Alternatives / Why / What it costs, in your own words
- [ ] `git add -A && git commit -m "Domain types and build configuration"` — your first commit
- [ ] Delete `.DS_Store` and add it to `.gitignore`

---

## Glossary

| Term | Meaning |
|---|---|
| **Translation unit** | One `.cpp` plus everything it includes, after preprocessing |
| **Declaration** | States that something exists and its shape. May repeat. |
| **Definition** | Provides the actual thing. Exactly once per program (ODR). |
| **ODR** | One Definition Rule |
| **Header** | `.hpp` file of declarations, included by many `.cpp` files |
| **`#pragma once`** | Prevents a header being pasted twice into one translation unit |
| **Namespace** | Named scope preventing name collisions |
| **Type alias** | New name for an existing type (`using X = Y;`). Not a distinct type. |
| **`constexpr`** | Evaluable at compile time |
| **`inline`** | "This definition may appear in multiple translation units" — not about speed |
| **`const` member function** | Promises not to modify the object |
| **`noexcept`** | Promises never to throw |
| **Scoped enum** | `enum class` — no scope pollution, no implicit int conversion |
| **Default member initialiser** | `= 0` in a struct field; guarantees a defined initial state |
| **Sentinel value** | A reserved value meaning "none" (here, `kInvalidPrice`) |
| **Sanitizer** | Runtime instrumentation catching memory and UB bugs |
| **Undefined reference** | Linker error: declared but never defined, or `.cpp` missing from the build |
