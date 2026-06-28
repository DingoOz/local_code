cmake_minimum_required(VERSION 3.16)
project(local_code CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
set(CURSES_NEED_WIDE TRUE)       # link ncursesw so the TUI renders UTF-8 (em
                                 # dashes etc.) instead of mojibake
find_package(Curses REQUIRED)   # ncurses, for the TUI
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)  # for the GPU monitor thread

add_executable(local_code
  src/main.cpp
  src/config.cpp
  src/ollama_client.cpp
  src/conversation.cpp
  src/tools.cpp
  src/agent.cpp
  src/plain_console.cpp
  src/gpu_monitor.cpp
  src/tui.cpp
  src/web_search.cpp
  src/project.cpp
  src/permissions.cpp
  src/undo.cpp
)

target_include_directories(local_code PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party   # vendored nlohmann/json single header
  ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(local_code PRIVATE
  CURL::libcurl
  ${CURSES_LIBRARIES}
  Threads::Threads
)

target_compile_options(local_code PRIVATE -Wall -Wextra)

# Deterministic unit test for the context-window guard (no network).
enable_testing()
add_executable(test_conversation tests/test_conversation.cpp src/conversation.cpp)
target_include_directories(test_conversation PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME conversation COMMAND test_conversation)

add_executable(test_parse tests/test_parse.cpp src/tools.cpp src/config.cpp
               src/web_search.cpp src/permissions.cpp src/undo.cpp)
target_include_directories(test_parse PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_parse PRIVATE CURL::libcurl)
add_test(NAME parse COMMAND test_parse)

add_executable(test_native tests/test_native.cpp src/tools.cpp src/config.cpp
               src/web_search.cpp src/permissions.cpp src/undo.cpp)
target_include_directories(test_native PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_native PRIVATE CURL::libcurl)
add_test(NAME native COMMAND test_native)

add_executable(test_features tests/test_features.cpp src/tools.cpp src/config.cpp
               src/web_search.cpp src/permissions.cpp src/undo.cpp)
target_include_directories(test_features PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_features PRIVATE CURL::libcurl)
add_test(NAME features COMMAND test_features)

add_executable(test_filter tests/test_filter.cpp)
add_test(NAME filter COMMAND test_filter)

add_executable(test_web tests/test_web.cpp src/web_search.cpp)
target_include_directories(test_web PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_web PRIVATE CURL::libcurl)
add_test(NAME web COMMAND test_web)
