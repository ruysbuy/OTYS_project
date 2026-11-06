cmake_minimum_required(VERSION 3.20)
project(minidocker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

set(CORE_SOURCES
    src/util.cpp
    src/container.cpp
    src/cgroups.cpp
    src/pty_relay.cpp
    src/store.cpp
    src/daemon.cpp
)

add_library(minidocker_core STATIC ${CORE_SOURCES})
target_include_directories(minidocker_core PUBLIC include)
target_link_libraries(minidocker_core PUBLIC nlohmann_json::nlohmann_json util pthread)
target_compile_options(minidocker_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Клиент (main.cpp лежит прямо в корне)
add_executable(minidocker main.cpp)
target_link_libraries(minidocker PRIVATE minidocker_core)

# Демон
add_executable(minidockerd src/daemon_main.cpp)
target_link_libraries(minidockerd PRIVATE minidocker_core)

install(TARGETS minidocker minidockerd RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME integration_container
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration_container.sh
                 $<TARGET_FILE:minidocker> $<TARGET_FILE:minidockerd>)
add_test(NAME daemon_protocol
         COMMAND ${CMAKE_SOURCE_DIR}/tests/daemon_protocol.sh
                 $<TARGET_FILE:minidocker> $<TARGET_FILE:minidockerd>)