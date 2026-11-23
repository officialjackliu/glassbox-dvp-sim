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

# Core library: everything except the CLI entry point.
add_library(glassbox_core STATIC
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    src/protocol/coordinator.cpp
    src/protocol/participant.cpp
    src/scenarios/protocol_system.cpp
    src/scenarios/scenario.cpp
    src/scenarios/scenario_library.cpp
    src/scenarios/invariant_checker.cpp
    src/trace/trace_format.cpp
)
target_include_directories(glassbox_core PUBLIC include)

# Front end: the glass-box terminal player. It reads the core through its
# public API and never changes it.
add_library(glassbox_cli STATIC
    src/cli/recording.cpp
    src/cli/narrator.cpp
    src/cli/pacing.cpp
    src/cli/screen.cpp
    src/cli/terminal.cpp
    src/cli/guide.cpp
)
target_include_directories(glassbox_cli PUBLIC include)
target_link_libraries(glassbox_cli PUBLIC glassbox_core)

add_executable(glassbox-dvp-sim
    src/main.cpp
)
target_link_libraries(glassbox-dvp-sim PRIVATE glassbox_cli)
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

add_executable(protocol_integration_test
    src/simulator.cpp
    src/domain/local_ledger.cpp
    src/domain/trade.cpp
    src/domain/durable_log.cpp
    src/domain/replay.cpp
    src/protocol/messages.cpp
    src/protocol/coordinator.cpp
    src/protocol/participant.cpp
    src/scenarios/protocol_system.cpp
    tests/protocol_integration_test.cpp
)
target_include_directories(protocol_integration_test PRIVATE include)
add_test(NAME protocol_integration_test COMMAND protocol_integration_test)

add_executable(scenario_s1_test tests/scenario_s1_test.cpp)
target_link_libraries(scenario_s1_test PRIVATE glassbox_core)
add_test(NAME scenario_s1_test COMMAND scenario_s1_test)

# CLI smoke tests (minimal runner, Task 15).
add_test(NAME cli_list COMMAND glassbox-dvp-sim --list)
set_tests_properties(cli_list PROPERTIES PASS_REGULAR_EXPRESSION "S1  Happy path")
add_test(NAME cli_s1 COMMAND glassbox-dvp-sim --scenario S1 --seed 1)
set_tests_properties(cli_s1 PROPERTIES PASS_REGULAR_EXPRESSION "scenario=S1 seed=1 quiesced=yes")
add_test(NAME cli_s1_final_state COMMAND glassbox-dvp-sim --scenario S1 --seed 7)
set_tests_properties(cli_s1_final_state PROPERTIES PASS_REGULAR_EXPRESSION
    "final BANK_A cash_total=97512500 cash_locked=0 bonds_total=25025 bonds_locked=0 trade1=COMMITTED")
add_test(NAME cli_s1_trace_line COMMAND glassbox-dvp-sim --scenario S1)
set_tests_properties(cli_s1_trace_line PROPERTIES PASS_REGULAR_EXPRESSION
    "t=144400 STATE_CHANGE COORD trade=1 detail=COMMITTING->DONE @Coordinator::on_ack")
add_test(NAME cli_unknown_scenario COMMAND glassbox-dvp-sim --scenario S99)
set_tests_properties(cli_unknown_scenario PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_bad_seed COMMAND glassbox-dvp-sim --scenario S1 --seed x1)
set_tests_properties(cli_bad_seed PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_no_args COMMAND glassbox-dvp-sim)
set_tests_properties(cli_no_args PROPERTIES WILL_FAIL TRUE)

add_executable(scenario_vote_no_test tests/scenario_vote_no_test.cpp)
target_link_libraries(scenario_vote_no_test PRIVATE glassbox_core)
add_test(NAME scenario_vote_no_test COMMAND scenario_vote_no_test)

add_executable(scenario_timeout_test tests/scenario_timeout_test.cpp)
target_link_libraries(scenario_timeout_test PRIVATE glassbox_core)
add_test(NAME scenario_timeout_test COMMAND scenario_timeout_test)

add_executable(scenario_lock_conflict_test tests/scenario_lock_conflict_test.cpp)
target_link_libraries(scenario_lock_conflict_test PRIVATE glassbox_core)
add_test(NAME scenario_lock_conflict_test COMMAND scenario_lock_conflict_test)

add_executable(crash_points_test tests/crash_points_test.cpp)
target_link_libraries(crash_points_test PRIVATE glassbox_core)
add_test(NAME crash_points_test COMMAND crash_points_test)

add_executable(scenario_participant_crash_test tests/scenario_participant_crash_test.cpp)
target_link_libraries(scenario_participant_crash_test PRIVATE glassbox_core)
add_test(NAME scenario_participant_crash_test COMMAND scenario_participant_crash_test)

add_executable(scenario_coordinator_failure_test tests/scenario_coordinator_failure_test.cpp)
target_link_libraries(scenario_coordinator_failure_test PRIVATE glassbox_core)
add_test(NAME scenario_coordinator_failure_test COMMAND scenario_coordinator_failure_test)

add_executable(scenario_partition_test tests/scenario_partition_test.cpp)
target_link_libraries(scenario_partition_test PRIVATE glassbox_core)
add_test(NAME scenario_partition_test COMMAND scenario_partition_test)

add_executable(invariant_checker_test tests/invariant_checker_test.cpp)
target_link_libraries(invariant_checker_test PRIVATE glassbox_core)
add_test(NAME invariant_checker_test COMMAND invariant_checker_test)

add_executable(cli_recording_test tests/cli_recording_test.cpp)
target_link_libraries(cli_recording_test PRIVATE glassbox_cli)
add_test(NAME cli_recording_test COMMAND cli_recording_test)

add_executable(cli_narrator_test tests/cli_narrator_test.cpp)
target_link_libraries(cli_narrator_test PRIVATE glassbox_cli)
add_test(NAME cli_narrator_test COMMAND cli_narrator_test)

add_executable(cli_player_test tests/cli_player_test.cpp)
target_link_libraries(cli_player_test PRIVATE glassbox_cli)
add_test(NAME cli_player_test COMMAND cli_player_test)

add_executable(cli_screen_test tests/cli_screen_test.cpp)
target_link_libraries(cli_screen_test PRIVATE glassbox_cli)
add_test(NAME cli_screen_test COMMAND cli_screen_test)

add_executable(cli_guide_test tests/cli_guide_test.cpp)
target_link_libraries(cli_guide_test PRIVATE glassbox_cli)
add_test(NAME cli_guide_test COMMAND cli_guide_test)
