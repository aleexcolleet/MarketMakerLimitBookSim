cmake_minimum_required(VERSION 3.16)
project(market_maker_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)

option(MMS_SANITIZE "Build with sanitizers" OFF)
if(MMS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(mms src/types.cpp src/order_book.cpp)
target_include_directories(mms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)