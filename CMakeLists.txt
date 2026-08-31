cmake_minimum_required(VERSION 3.16)
project(market_maker_limit_book_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings are errors. A matching engine that compiles with warnings is a
# matching engine with bugs that have not been found yet.
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
endif()

# Opt-in address and undefined-behaviour sanitizers:
#   cmake -B build-asan -DMMS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
option(MMS_SANITIZE "Build with address and UB sanitizers" OFF)
if(MMS_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
#  Core library
# ---------------------------------------------------------------------------
add_library(mms src/order_book.cpp
        include/mms/value_process.hpp)
target_include_directories(mms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
#  Tests
# ---------------------------------------------------------------------------
enable_testing()

# Two binaries, not one: two test files each need their own main(). Linking
# `mms` also propagates its PUBLIC include directory, which is how the test
# sources find <mms/...>. check.hpp is found by quoted include, relative to the
# including file.
add_executable(mms_tests tests/test_order_book.cpp)
target_link_libraries(mms_tests PRIVATE mms)
add_test(NAME order_book COMMAND mms_tests)

add_executable(mms_sim_tests tests/test_random.cpp)
target_link_libraries(mms_sim_tests PRIVATE mms)
add_test(NAME simulation COMMAND mms_sim_tests)
