cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Emits build/compile_commands.json, which is what gives an editor's C++
# IntelliSense (VSCode's cpptools or clangd) the -Iinclude flag it needs to
# resolve the project's quoted #include "order_book.hpp"-style headers.
# Without it, every header include is flagged as an error in the editor even
# though the code builds fine -- see .vscode/c_cpp_properties.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -O2)

include_directories(include)

add_library(order_book src/order_book.cpp src/matching_engine.cpp src/event_log.cpp src/snapshot.cpp src/wire.cpp src/server.cpp src/recovery.cpp)

add_executable(matching_engine_cli src/main.cpp)
target_link_libraries(matching_engine_cli order_book)

add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests order_book)

add_executable(benchmark_runner benchmark/benchmark.cpp)
target_link_libraries(benchmark_runner order_book)

add_executable(matching_engine_server src/server_main.cpp)
target_link_libraries(matching_engine_server order_book)
