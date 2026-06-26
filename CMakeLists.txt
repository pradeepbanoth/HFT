cmake_minimum_required(VERSION 3.16)
project(hft_backtest CXX)

# ── Compiler settings ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Default to Release for maximum performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g3>
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(
        /W4 /permissive-
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
endif()

# ── Include directories ───────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Interface library (header-only) ──────────────────────────────────────────
add_library(hft_headers INTERFACE)
target_include_directories(hft_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include)

# ── Test executable ───────────────────────────────────────────────────────────
add_executable(test_all
    tests/test_all.cpp)
target_link_libraries(test_all PRIVATE hft_headers)

# ── Example executables ───────────────────────────────────────────────────────
add_executable(binance_btc_mm
    examples/binance_btc_mm.cpp)
target_link_libraries(binance_btc_mm PRIVATE hft_headers)

add_executable(bybit_multiasset_mm
    examples/bybit_multiasset_mm.cpp)
target_link_libraries(bybit_multiasset_mm PRIVATE hft_headers)

# ── CTest integration ─────────────────────────────────────────────────────────
enable_testing()
add_test(NAME UnitTests COMMAND test_all)

# ── Install ───────────────────────────────────────────────────────────────────
install(DIRECTORY include/ DESTINATION include/hft)
install(TARGETS binance_btc_mm bybit_multiasset_mm
        RUNTIME DESTINATION bin)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")