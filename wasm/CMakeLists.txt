# BALANCER_META:
#   meta_version: 1
#   component: fatp-balancer-wasm
#   file_role: build_script
#   path: wasm/CMakeLists.txt
#   layer: Demo
#   summary: Emscripten build for the Phase 6 cluster-visualization WASM demo.
#   api_stability: demo_only

# ---------------------------------------------------------------------------
# fatp-balancer WASM demo — requires Emscripten toolchain
#
# Usage:
#   emcmake cmake .. -DFATP_INCLUDE_DIR=/path/to/FatP/include/fat_p \
#                    -DCMAKE_BUILD_TYPE=Release
#   cmake --build .
#   # Outputs: balancer.js + balancer.wasm in this build directory
#
# Then copy both files to demo/ and open demo/index.html in a browser.
# The demo automatically detects WASM and uses it; without it, it falls
# back to the pure-JavaScript simulation layer.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)
project(fatp-balancer-wasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Verify we're using the Emscripten toolchain
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This CMakeLists.txt must be configured with the Emscripten toolchain.\n"
        "Use: emcmake cmake <source-dir> ...\n"
        "Or:  cmake -DCMAKE_TOOLCHAIN_FILE=<emscripten>/cmake/Modules/Platform/Emscripten.cmake ..."
    )
endif()

# ---------------------------------------------------------------------------
# FAT-P include path (same resolution as the main build)
# ---------------------------------------------------------------------------
if(NOT DEFINED FATP_INCLUDE_DIR)
    set(FATP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../FatP/include/fat_p"
        CACHE PATH "Path to FAT-P include directory")
endif()

if(NOT EXISTS "${FATP_INCLUDE_DIR}/Expected.h")
    message(FATAL_ERROR
        "FAT-P headers not found at ${FATP_INCLUDE_DIR}.\n"
        "Set FATP_INCLUDE_DIR to the directory containing Expected.h.")
endif()
message(STATUS "FAT-P include dir: ${FATP_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# WASM executable — Emscripten compiles to .js + .wasm pair
# ---------------------------------------------------------------------------
add_executable(balancer
    ${CMAKE_CURRENT_SOURCE_DIR}/BalancerBindings.cpp
)

target_include_directories(balancer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../policies
    ${CMAKE_CURRENT_SOURCE_DIR}/../sim
    ${FATP_INCLUDE_DIR}
)

target_compile_features(balancer PRIVATE cxx_std_20)

target_compile_options(balancer PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Emscripten link flags
# ---------------------------------------------------------------------------
set_target_properties(balancer PROPERTIES
    OUTPUT_NAME   "balancer"
    SUFFIX        ".js"
)

target_link_options(balancer PRIVATE
    "--bind"                              # Enable embind
    "-s WASM=1"
    "-s MODULARIZE=1"
    "-s EXPORT_NAME=BalancerModule"
    "-s ALLOW_MEMORY_GROWTH=1"
    "-s INITIAL_MEMORY=33554432"          # 32 MB initial heap
    "-s EXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\"]"
    "-s USE_PTHREADS=0"                   # SimulatedNode uses std::thread;
                                          # disable for browser compat and use
                                          # the JS simulation layer instead.
    "-O2"
    "--no-entry"
)

# ---------------------------------------------------------------------------
# Post-build: copy output to demo/ for the HTML page to load
# ---------------------------------------------------------------------------
add_custom_command(TARGET balancer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:balancer>/balancer.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/../demo/balancer.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:balancer>/balancer.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../demo/balancer.wasm"
    COMMENT "Copying WASM artifacts to demo/"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "fatp-balancer WASM demo build configured")
message(STATUS "  Emscripten : ${EMSCRIPTEN_VERSION}")
message(STATUS "  Output     : balancer.js + balancer.wasm")
message(STATUS "  Demo       : ${CMAKE_CURRENT_SOURCE_DIR}/../demo/index.html")
