# cmake: declarative build — targets, sources, and link deps; generates make/ninja/vs projects.
cmake_minimum_required(VERSION 3.16)
project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# a build with no explicit type gets no optimization flags at all, which quietly
# makes every benchmark number meaningless. default to release; -DCMAKE_BUILD_TYPE
# still overrides. only applies to single-config generators (make/ninja).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(ORDER_BOOK_EXPORT_COMPILE_COMMANDS "Generate compile_commands.json" ON)
if(ORDER_BOOK_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# warnings live on an INTERFACE target rather than global add_compile_options so
# they apply to our code only. googletest is fetched as source and built in this
# tree; -Werror on third-party code means their warnings break our build.
add_library(ome_warnings INTERFACE)
target_compile_options(ome_warnings INTERFACE
  -Wall -Wextra -Werror
  -Wshadow            # a local shadowing a member is how "why didn't that assignment stick" bugs happen
  -Wconversion        # implicit narrowing matters once prices/quantities are fixed-width
  -Wsign-conversion
)

# OME_SANITIZE: "", "address", "undefined", "address,undefined", or "thread".
# sanitizers must instrument every translation unit AND the link step, so unlike
# warnings these go on globally — including googletest, which is correct: an
# uninstrumented library linked into an asan binary reports false negatives.
set(OME_SANITIZE "" CACHE STRING "Sanitizers: address, undefined, address,undefined, or thread")
if(OME_SANITIZE)
  # tsan maintains its own shadow memory layout and cannot coexist with asan.
  if(OME_SANITIZE MATCHES "thread" AND OME_SANITIZE MATCHES "address")
    message(FATAL_ERROR "OME_SANITIZE: thread and address are mutually exclusive")
  endif()
  add_compile_options(-fsanitize=${OME_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${OME_SANITIZE})
  message(STATUS "sanitizers enabled: ${OME_SANITIZE}")
endif()

add_library(order_book STATIC
  src/net/tcp_server.cpp
  src/order_book.cpp
  src/matching_engine.cpp
  src/lobster_parser.cpp
  src/lobster_validator.cpp
  src/lobster_orderbook_converter.cpp
)
target_include_directories(order_book PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# PRIVATE: consumers of order_book inherit its headers, not its warning flags.
target_link_libraries(order_book PRIVATE ome_warnings)

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE order_book ome_warnings)

add_executable(lobster_replay src/lobster_replay.cpp)
target_link_libraries(lobster_replay PRIVATE order_book ome_warnings)

add_executable(gateway src/gateway_main.cpp)
target_link_libraries(gateway PRIVATE order_book ome_warnings)

# The load generator needs threads regardless of BUILD_TESTING.
find_package(Threads REQUIRED)
add_executable(loadgen bench/loadgen.cpp)
target_link_libraries(loadgen PRIVATE order_book ome_warnings Threads::Threads)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Threads::Threads is already found above for loadgen.
  add_executable(tests
    tests/test_order_book.cpp
    tests/test_matching.cpp
    tests/test_lobster.cpp
    tests/test_spsc.cpp
    tests/test_reject_reason.cpp
    tests/test_protocol.cpp
    tests/test_framing.cpp
    tests/test_tcp_server.cpp
    tests/test_session.cpp
    tests/test_pipeline.cpp
    tests/test_risk.cpp
    tests/test_broadcast.cpp
    tests/test_notifier.cpp
    tests/test_wal.cpp
    tests/test_recovery.cpp
  )
  target_link_libraries(tests PRIVATE order_book ome_warnings GTest::gtest_main Threads::Threads)

  include(GoogleTest)
  gtest_discover_tests(tests)
endif()
