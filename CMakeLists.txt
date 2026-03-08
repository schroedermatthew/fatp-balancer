# BALANCER_META:
#   meta_version: 1
#   component: fatp-balancer
#   file_role: build_script
#   path: CMakeLists.txt
#   layer: Testing
#   summary: Root CMake build for fatp-balancer Phase 5 — all targets, sanitizer configs, test registration.
#   api_stability: in_work

cmake_minimum_required(VERSION 3.20)
project(fatp-balancer LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20 — hard requirement
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# FAT-P include path
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
# Interface library: balancer headers (transferable core)
# ---------------------------------------------------------------------------
add_library(balancer_headers INTERFACE)
target_include_directories(balancer_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/policies
    ${CMAKE_SOURCE_DIR}
    ${FATP_INCLUDE_DIR}
)
target_compile_features(balancer_headers INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# Interface library: simulation layer
# ---------------------------------------------------------------------------
add_library(balancer_sim INTERFACE)
target_include_directories(balancer_sim INTERFACE
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(balancer_sim INTERFACE balancer_headers)

# ---------------------------------------------------------------------------
# Compiler warning flags
# ---------------------------------------------------------------------------
set(BALANCER_WARN_FLAGS)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(BALANCER_WARN_FLAGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(BALANCER_WARN_FLAGS /W4 /permissive- /Zc:preprocessor)
endif()

# ---------------------------------------------------------------------------
# Sanitizer options (apply to all test targets)
# ---------------------------------------------------------------------------
option(BALANCER_ENABLE_ASAN  "Build tests with AddressSanitizer"    OFF)
option(BALANCER_ENABLE_TSAN  "Build tests with ThreadSanitizer"     OFF)
option(BALANCER_ENABLE_UBSAN "Build tests with UBSanitizer"         OFF)

set(BALANCER_SANITIZER_FLAGS)
if(BALANCER_ENABLE_ASAN)
    list(APPEND BALANCER_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(BALANCER_ENABLE_TSAN)
    list(APPEND BALANCER_SANITIZER_FLAGS -fsanitize=thread)
endif()
if(BALANCER_ENABLE_UBSAN)
    list(APPEND BALANCER_SANITIZER_FLAGS -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Helper: create a functional test executable (single source, has its own main)
# ---------------------------------------------------------------------------
function(balancer_add_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE balancer_sim)
    target_compile_definitions(${target_name} PRIVATE ENABLE_TEST_APPLICATION)
    target_compile_options(${target_name} PRIVATE
        ${BALANCER_WARN_FLAGS}
        ${BALANCER_SANITIZER_FLAGS}
    )
    if(BALANCER_SANITIZER_FLAGS)
        target_link_options(${target_name} PRIVATE ${BALANCER_SANITIZER_FLAGS})
    endif()
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# ---------------------------------------------------------------------------
# Helper: compile-only test (no main, verifies header inclusion)
# ---------------------------------------------------------------------------
function(balancer_add_compile_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE balancer_headers)
    target_compile_options(${target_name} PRIVATE ${BALANCER_WARN_FLAGS})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# ---------------------------------------------------------------------------
# Helper: compile-only test that requires sim layer headers
# ---------------------------------------------------------------------------
function(balancer_add_sim_compile_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE balancer_sim)
    target_compile_options(${target_name} PRIVATE ${BALANCER_WARN_FLAGS})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# ---------------------------------------------------------------------------
# Enable CTest
# ---------------------------------------------------------------------------
enable_testing()

# ---------------------------------------------------------------------------
# Transferability gate — runs before anything compiles.
# Verifies sim/ is never included by include/balancer/ or policies/.
# ---------------------------------------------------------------------------
set(TRANSFERABILITY_GATE_PASSED TRUE)
file(GLOB_RECURSE CORE_HEADERS
    "${CMAKE_SOURCE_DIR}/include/balancer/*.h"
    "${CMAKE_SOURCE_DIR}/policies/*.h"
)
foreach(hdr IN LISTS CORE_HEADERS)
    file(READ "${hdr}" hdr_content)
    if(hdr_content MATCHES "#include[[:space:]]+\"sim/")
        message(WARNING "TRANSFERABILITY VIOLATION: ${hdr} includes a sim/ header")
        set(TRANSFERABILITY_GATE_PASSED FALSE)
    endif()
endforeach()
if(NOT TRANSFERABILITY_GATE_PASSED)
    message(FATAL_ERROR "Transferability gate failed. See warnings above.")
endif()
message(STATUS "Transferability gate: PASSED")

# ---------------------------------------------------------------------------
# Hygiene tests — compile-only (one per public header)
# ---------------------------------------------------------------------------
balancer_add_compile_test(test_include_all
    tests/test_include_all_balancer_headers.cpp)

# Core interface headers
balancer_add_compile_test(test_Job_HeaderSelfContained
    tests/test_Job_HeaderSelfContained.cpp)
balancer_add_compile_test(test_INode_HeaderSelfContained
    tests/test_INode_HeaderSelfContained.cpp)
balancer_add_compile_test(test_LoadMetrics_HeaderSelfContained
    tests/test_LoadMetrics_HeaderSelfContained.cpp)
balancer_add_compile_test(test_BalancerConfig_HeaderSelfContained
    tests/test_BalancerConfig_HeaderSelfContained.cpp)
balancer_add_compile_test(test_CostModel_HeaderSelfContained
    tests/test_CostModel_HeaderSelfContained.cpp)
balancer_add_compile_test(test_ClusterView_HeaderSelfContained
    tests/test_ClusterView_HeaderSelfContained.cpp)
balancer_add_compile_test(test_ISchedulingPolicy_HeaderSelfContained
    tests/test_ISchedulingPolicy_HeaderSelfContained.cpp)
balancer_add_compile_test(test_AdmissionControl_HeaderSelfContained
    tests/test_AdmissionControl_HeaderSelfContained.cpp)
balancer_add_compile_test(test_AgingEngine_HeaderSelfContained
    tests/test_AgingEngine_HeaderSelfContained.cpp)
balancer_add_compile_test(test_Balancer_HeaderSelfContained
    tests/test_Balancer_HeaderSelfContained.cpp)

# Phase 1 policy headers
balancer_add_compile_test(test_RoundRobin_HeaderSelfContained
    tests/test_RoundRobin_HeaderSelfContained.cpp)
balancer_add_compile_test(test_LeastLoaded_HeaderSelfContained
    tests/test_LeastLoaded_HeaderSelfContained.cpp)

# Phase 2 policy headers
balancer_add_compile_test(test_WeightedCapacity_HeaderSelfContained
    tests/test_WeightedCapacity_HeaderSelfContained.cpp)
balancer_add_compile_test(test_ShortestJobFirst_HeaderSelfContained
    tests/test_ShortestJobFirst_HeaderSelfContained.cpp)
balancer_add_compile_test(test_EarliestDeadlineFirst_HeaderSelfContained
    tests/test_EarliestDeadlineFirst_HeaderSelfContained.cpp)
balancer_add_compile_test(test_Composite_HeaderSelfContained
    tests/test_Composite_HeaderSelfContained.cpp)

# Phase 4 policy headers
balancer_add_compile_test(test_WorkStealing_HeaderSelfContained
    tests/test_WorkStealing_HeaderSelfContained.cpp)

# Phase 5 policy and learning headers
balancer_add_compile_test(test_AffinityMatrix_HeaderSelfContained
    tests/test_AffinityMatrix_HeaderSelfContained.cpp)
balancer_add_compile_test(test_AffinityRouting_HeaderSelfContained
    tests/test_AffinityRouting_HeaderSelfContained.cpp)

# Phase 7 feature management headers
balancer_add_test(test_BalancerFeatures_HeaderSelfContained
    tests/test_BalancerFeatures_HeaderSelfContained.cpp)

# Phase 7 sim headers
balancer_add_test(test_TelemetrySnapshot_HeaderSelfContained
    tests/test_TelemetrySnapshot_HeaderSelfContained.cpp)

# Phase 7 Step 6.4 — FeatureSupervisor
balancer_add_test(test_FeatureSupervisor_HeaderSelfContained
    tests/test_FeatureSupervisor_HeaderSelfContained.cpp)
balancer_add_test(test_feature_supervisor
    tests/test_feature_supervisor.cpp)

# Phase 7 Step 6.5 — TelemetryAdvisor
balancer_add_test(test_TelemetryAdvisor_HeaderSelfContained
    tests/test_TelemetryAdvisor_HeaderSelfContained.cpp)
balancer_add_test(test_telemetry_advisor
    tests/test_telemetry_advisor.cpp)

# Phase 7 Step 6.6 — Telemetry integration (wiring + JSON panel)
balancer_add_test(test_telemetry_integration
    tests/test_telemetry_integration.cpp)

# Phase 8 — Debt resolution (DEBT-001/002/003/004/006)
balancer_add_test(test_PolicyFactory_HeaderSelfContained
    tests/test_PolicyFactory_HeaderSelfContained.cpp)
balancer_add_test(test_policy_factory
    tests/test_policy_factory.cpp)
balancer_add_test(test_phase8_debt
    tests/test_phase8_debt.cpp)

# Phase 9 — DEBT-005: HoldingQueue implementation
balancer_add_test(test_HoldingQueue_HeaderSelfContained
    tests/test_HoldingQueue_HeaderSelfContained.cpp)
balancer_add_test(test_holding_queue
    tests/test_holding_queue.cpp)

balancer_add_test(test_simulated_node_override
    tests/test_simulated_node_override.cpp)

# Phase 4 sim headers
balancer_add_sim_compile_test(test_FaultInjector_HeaderSelfContained
    tests/test_FaultInjector_HeaderSelfContained.cpp)

# ---------------------------------------------------------------------------
# Functional tests — individual executables
# ---------------------------------------------------------------------------
balancer_add_test(test_cost_model    tests/test_cost_model.cpp)
balancer_add_test(test_policies      tests/test_policies.cpp)
balancer_add_test(test_admission     tests/test_admission.cpp)
balancer_add_test(test_node_fsm      tests/test_node_fsm.cpp)
balancer_add_test(test_balancer_core tests/test_balancer_core.cpp)

# Phase 3 functional tests
balancer_add_test(test_aging         tests/test_aging.cpp)

# Phase 4 functional tests
balancer_add_test(test_fault_scenarios tests/test_fault_scenarios.cpp)

# Phase 5 functional tests
balancer_add_test(test_affinity_matrix tests/test_affinity_matrix.cpp)

# Phase 7 probe test — uses FeatureManager from FAT-P
balancer_add_test(test_feature_manager_cascade_probe
    tests/test_feature_manager_cascade_probe.cpp)

# ---------------------------------------------------------------------------
# Test orchestrator — links all suites into one binary
# ---------------------------------------------------------------------------
add_executable(test_balancer
    tests/test_balancer.cpp
    tests/test_admission.cpp
    tests/test_affinity_matrix.cpp
    tests/test_aging.cpp
    tests/test_cost_model.cpp
    tests/test_policies.cpp
    tests/test_node_fsm.cpp
    tests/test_balancer_core.cpp
    tests/test_fault_scenarios.cpp
    tests/test_feature_manager_cascade_probe.cpp
)
target_link_libraries(test_balancer PRIVATE balancer_sim)
target_compile_options(test_balancer PRIVATE
    ${BALANCER_WARN_FLAGS}
    ${BALANCER_SANITIZER_FLAGS}
)
if(BALANCER_SANITIZER_FLAGS)
    target_link_options(test_balancer PRIVATE ${BALANCER_SANITIZER_FLAGS})
endif()
add_test(NAME test_balancer COMMAND test_balancer)

# ---------------------------------------------------------------------------
# Phase 6: WASM demo subdirectory (requires Emscripten toolchain)
# ---------------------------------------------------------------------------
# To build the WASM demo:
#   cd wasm && emcmake cmake . -DFATP_INCLUDE_DIR=... && cmake --build .
# The wasm/CMakeLists.txt is a standalone project; it is NOT included here
# because it requires the Emscripten toolchain and is incompatible with the
# native test build. Include it only when explicitly requested.
option(BALANCER_BUILD_WASM_DEMO "Build the Phase 6 WASM demo (requires Emscripten)" OFF)
if(BALANCER_BUILD_WASM_DEMO)
    if(NOT EMSCRIPTEN)
        message(WARNING "BALANCER_BUILD_WASM_DEMO requires Emscripten. Use: emcmake cmake ..")
    else()
        add_subdirectory(wasm)
    endif()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "fatp-balancer Phase 11 build configured")
message(STATUS "  C++ standard : C++20")
message(STATUS "  FAT-P dir    : ${FATP_INCLUDE_DIR}")
message(STATUS "  ASan         : ${BALANCER_ENABLE_ASAN}")
message(STATUS "  TSan         : ${BALANCER_ENABLE_TSAN}")
message(STATUS "  UBSan        : ${BALANCER_ENABLE_UBSAN}")
message(STATUS "Run: cmake --build . && ctest --output-on-failure")
