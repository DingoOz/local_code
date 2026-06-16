cmake_minimum_required(VERSION 3.16)
project(local_code CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)

# GNU readline (no standard CMake module; locate manually)
find_path(READLINE_INCLUDE_DIR readline/readline.h)
find_library(READLINE_LIBRARY NAMES readline)
if(NOT READLINE_INCLUDE_DIR OR NOT READLINE_LIBRARY)
  message(FATAL_ERROR "GNU readline not found (install libreadline-dev)")
endif()

add_executable(local_code
  src/main.cpp
  src/config.cpp
  src/ollama_client.cpp
  src/conversation.cpp
  src/tools.cpp
  src/agent.cpp
)

target_include_directories(local_code PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party   # vendored nlohmann/json single header
  ${READLINE_INCLUDE_DIR}
)

target_link_libraries(local_code PRIVATE
  CURL::libcurl
  ${READLINE_LIBRARY}
)

target_compile_options(local_code PRIVATE -Wall -Wextra)

# Deterministic unit test for the context-window guard (no network).
enable_testing()
add_executable(test_conversation tests/test_conversation.cpp src/conversation.cpp)
target_include_directories(test_conversation PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME conversation COMMAND test_conversation)

add_executable(test_parse tests/test_parse.cpp src/tools.cpp src/config.cpp)
target_include_directories(test_parse PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME parse COMMAND test_parse)

add_executable(test_filter tests/test_filter.cpp)
add_test(NAME filter COMMAND test_filter)
