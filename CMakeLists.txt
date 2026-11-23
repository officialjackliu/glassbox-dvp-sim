cmake_minimum_required(VERSION 3.20)

project(glassbox-dvp-sim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

option(GLASSBOX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(GLASSBOX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(GLASSBOX_ENABLE_ASAN OR GLASSBOX_ENABLE_UBSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GNU C++")
    endif()
endif()

if(GLASSBOX_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(GLASSBOX_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

add_executable(glassbox-dvp-sim
    src/main.cpp
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    src/protocol/coordinator.cpp
    src/protocol/participant.cpp
)

target_include_directories(glassbox-dvp-sim PRIVATE include)

enable_testing()

# Small executable checks cover core types and serve as CTest smoke coverage.
add_executable(smoke_test
    tests/smoke_test.cpp
)

target_include_directories(smoke_test PRIVATE include)
add_test(NAME smoke_test COMMAND smoke_test)

# Component test executables mirror implemented source modules.
add_executable(event_queue_test
    tests/event_queue_test.cpp
)

target_include_directories(event_queue_test PRIVATE include)
add_test(NAME event_queue_test COMMAND event_queue_test)

add_executable(virtual_clock_test
    tests/virtual_clock_test.cpp
)

target_include_directories(virtual_clock_test PRIVATE include)
add_test(NAME virtual_clock_test COMMAND virtual_clock_test)

add_executable(deterministic_rng_test
    tests/deterministic_rng_test.cpp
)

target_include_directories(deterministic_rng_test PRIVATE include)
add_test(NAME deterministic_rng_test COMMAND deterministic_rng_test)

# Directed-link, partition, node-down, and drop scheduling tests.
add_executable(network_model_test
    tests/network_model_test.cpp
)

target_include_directories(network_model_test PRIVATE include)
add_test(NAME network_model_test COMMAND network_model_test)

add_executable(simulator_test
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    tests/simulator_test.cpp
)

target_include_directories(simulator_test PRIVATE include)
add_test(NAME simulator_test COMMAND simulator_test)

add_executable(node_purity_test
    tests/node_purity_test.cpp
)

target_include_directories(node_purity_test PRIVATE include)
add_test(NAME node_purity_test COMMAND node_purity_test)
set_tests_properties(node_purity_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(timers_test
    tests/timers_test.cpp
)

target_include_directories(timers_test PRIVATE include)
add_test(NAME timers_test COMMAND timers_test)

add_executable(local_ledger_test
    src/domain/local_ledger.cpp
    tests/local_ledger_test.cpp
)

target_include_directories(local_ledger_test PRIVATE include)
add_test(NAME local_ledger_test COMMAND local_ledger_test)
set_tests_properties(local_ledger_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(trade_test
    src/domain/trade.cpp
    tests/trade_test.cpp
)

target_include_directories(trade_test PRIVATE include)
add_test(NAME trade_test COMMAND trade_test)
set_tests_properties(trade_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(durable_log_test
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    tests/durable_log_test.cpp
)
target_include_directories(durable_log_test PRIVATE include)
add_test(NAME durable_log_test COMMAND durable_log_test)
set_tests_properties(durable_log_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(replay_test
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    tests/replay_test.cpp
)
target_include_directories(replay_test PRIVATE include)
add_test(NAME replay_test COMMAND replay_test)
set_tests_properties(replay_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(messages_test
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    tests/messages_test.cpp
)
target_include_directories(messages_test PRIVATE include)
add_test(NAME messages_test COMMAND messages_test)

add_executable(coordinator_test
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    src/protocol/coordinator.cpp
    tests/coordinator_test.cpp
)
target_include_directories(coordinator_test PRIVATE include)
add_test(NAME coordinator_test COMMAND coordinator_test)

add_executable(participant_test
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    src/protocol/participant.cpp
    tests/participant_test.cpp
)
target_include_directories(participant_test PRIVATE include)
add_test(NAME participant_test COMMAND participant_test)
