# cmake: declarative build — targets, sources, and link deps; generates make/ninja/vs projects.
cmake_minimum_required(VERSION 3.16)
project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDER_BOOK_EXPORT_COMPILE_COMMANDS "Generate compile_commands.json" ON)
if(ORDER_BOOK_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_library(order_book STATIC
  src/order_book.cpp
  src/matching_engine.cpp
  src/lobster_parser.cpp
  src/lobster_validator.cpp
  src/lobster_orderbook_converter.cpp
)
target_include_directories(order_book PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE order_book)

add_executable(lobster_replay src/lobster_replay.cpp)
target_link_libraries(lobster_replay PRIVATE order_book)

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

  add_executable(tests
    tests/test_order_book.cpp
    tests/test_matching.cpp
    tests/test_lobster.cpp
    tests/test_spsc.cpp
  )
  target_link_libraries(tests PRIVATE order_book GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(tests)
endif()
