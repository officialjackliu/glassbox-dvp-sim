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
)

target_include_directories(glassbox-dvp-sim PRIVATE include)

enable_testing()

add_executable(smoke_test
    tests/smoke_test.cpp
)

target_include_directories(smoke_test PRIVATE include)
add_test(NAME smoke_test COMMAND smoke_test)
