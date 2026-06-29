cmake_minimum_required(VERSION 3.16)
project(hft_backtest CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g3>
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /permissive-
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>)
endif()

# ── Headers ───────────────────────────────────────────────────────────────────
add_library(hft_headers INTERFACE)
target_include_directories(hft_headers INTERFACE ${CMAKE_SOURCE_DIR}/include)

# ── Thread support (for Optimizer multi-threading) ────────────────────────────
find_package(Threads REQUIRED)

# ── Helper macro ──────────────────────────────────────────────────────────────
function(hft_executable name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE hft_headers Threads::Threads)
endfunction()

# ── Tests ─────────────────────────────────────────────────────────────────────
hft_executable(test_all       tests/test_all.cpp)
hft_executable(test_advanced  tests/test_advanced.cpp)
hft_executable(test_modules   tests/test_modules.cpp)

# ── Examples ──────────────────────────────────────────────────────────────────
hft_executable(binance_btc_mm       examples/binance_btc_mm.cpp)
hft_executable(bybit_multiasset_mm  examples/bybit_multiasset_mm.cpp)
hft_executable(optimize_and_analyze examples/optimize_and_analyze.cpp)
hft_executable(production_workflow  examples/production_workflow.cpp)

# ── CTest ─────────────────────────────────────────────────────────────────────
enable_testing()
add_test(NAME CoreTests     COMMAND test_all)
add_test(NAME AdvancedTests COMMAND test_advanced)
add_test(NAME ModuleTests   COMMAND test_modules)

# ── Install ───────────────────────────────────────────────────────────────────
install(DIRECTORY include/ DESTINATION include/hft)
install(TARGETS binance_btc_mm bybit_multiasset_mm optimize_and_analyze
        RUNTIME DESTINATION bin)

message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")