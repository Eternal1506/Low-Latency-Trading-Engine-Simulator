cmake_minimum_required(VERSION 3.18)
project(lxts CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ──────────────────────────────────────────
add_compile_options(
    -O3
    -march=native
    -Wall -Wextra -Wpedantic
    -pthread
)

# ── Dependencies ────────────────────────────────────────────
find_package(Threads REQUIRED)

# hiredis (install: apt install libhiredis-dev  OR  brew install hiredis)
find_library(HIREDIS_LIB NAMES hiredis
    PATHS C:/msys64/ucrt64/lib C:/msys64/ucrt64/bin
    REQUIRED
)
find_path(HIREDIS_INCLUDE NAMES hiredis/hiredis.h
    PATHS C:/msys64/ucrt64/include
    REQUIRED
)

# ── Engine executable ───────────────────────────────────────
add_executable(lxts_engine lxts/main.cpp)

target_include_directories(lxts_engine PRIVATE
    ${HIREDIS_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/lxts
)
target_link_libraries(lxts_engine PRIVATE
    ${HIREDIS_LIB}
    Threads::Threads
)

if (WIN32)
    target_link_libraries(lxts_engine PRIVATE ws2_32)
endif()

# ── Latency benchmark ───────────────────────────────────────
add_executable(bench_latency lxts/analysis/bench_latency.cpp)
target_include_directories(bench_latency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lxts
)
target_link_libraries(bench_latency PRIVATE Threads::Threads)

# ── Install ─────────────────────────────────────────────────
install(TARGETS lxts_engine DESTINATION bin)
