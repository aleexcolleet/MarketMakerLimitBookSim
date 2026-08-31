#pragma once

// Minimal check harness, shared by the test binaries. No external
// dependencies, so this builds anywhere with a compiler and CMake.

#include <cstdio>
#include <string>
#include <vector>

namespace check_harness {

    // `inline` on a namespace-scope variable is C++17. It means: this may be
    // defined in several translation units, and all of them refer to *one* object.
    // Without it, a variable defined in a header is defined once per including .cpp
    // and the linker rejects the duplicates — which is why header-only libraries
    // used to contort themselves around function-local statics.
    inline int checks_run = 0;
    inline std::vector<std::string> failures;

    inline void record(bool ok, const char* expr, const char* file, int line) {
        ++checks_run;
        if (!ok) {
            failures.push_back(std::string(file) + ":" + std::to_string(line) + "  " + expr);
        }
    }

    inline int report() {
        if (failures.empty()) {
            std::printf("%d checks passed\n", checks_run);
            return 0;
        }
        std::printf("%zu of %d checks FAILED\n", failures.size(), checks_run);
        for (const auto& f : failures) std::printf("  %s\n", f.c_str());
        return 1;
    }

}  // namespace check_harness

// Stays a macro, and has to. __FILE__ and __LINE__ must expand at the call
// site, and #expr — stringifying the expression — is only available to the
// preprocessor. A function would report the harness's own line number on every
// failure.
#define CHECK(expr) \
    ::check_harness::record(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
