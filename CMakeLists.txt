cmake_minimum_required(VERSION 3.16)
project(mistral-cli CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------
# Platform detection
# -----------------------------------------------

if(APPLE)
    set(DRIPCORE_EVENT_LOOP_SOURCE
        references/dripcore-dev/lib/dripcore/source/event_loop_kqueue.cpp)
elseif(UNIX)
    set(DRIPCORE_EVENT_LOOP_SOURCE
        references/dripcore-dev/lib/dripcore/source/event_loop_epoll.cpp)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# -----------------------------------------------
# Third-party: dripcore (built from source)
# -----------------------------------------------

add_library(dripcore STATIC
    references/dripcore-dev/lib/dripcore/source/coroutine.cpp
    references/dripcore-dev/lib/dripcore/source/executor.cpp
    references/dripcore-dev/lib/dripcore/source/scheduler.cpp
    references/dripcore-dev/lib/dripcore/source/yield_awaiter.cpp
    references/dripcore-dev/lib/dripcore/source/io_awaiter.cpp
    references/dripcore-dev/lib/dripcore/source/socket.cpp
    ${DRIPCORE_EVENT_LOOP_SOURCE}
)

target_compile_features(dripcore PUBLIC cxx_std_20)

target_include_directories(dripcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/references/dripcore-dev/lib/dripcore/include
)

# -----------------------------------------------
# Third-party: stunning-waddle (HTTP/2 library)
# -----------------------------------------------

add_subdirectory(
    references/stunning-waddle-examples/vendor/stunning-waddle
    stunning-waddle-build
)

# -----------------------------------------------
# Third-party: isocline (readline library)
# -----------------------------------------------

add_subdirectory(
    references/mimir_ex/third_party/isocline
    isocline-build
)

# -----------------------------------------------
# Third-party: OpenSSL
# -----------------------------------------------

find_package(OpenSSL REQUIRED)

# -----------------------------------------------
# Third-party: nlohmann/json (header-only)
# -----------------------------------------------

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# -----------------------------------------------
# Core library
# -----------------------------------------------

add_library(mistral-core STATIC
    lib/http/tls_connection.cpp
    lib/api/mistral_api.cpp
    lib/tools/file_tools.cpp
    lib/console/console_task.cpp
)

target_compile_features(mistral-core PUBLIC cxx_std_20)

target_include_directories(mistral-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

target_link_libraries(mistral-core PUBLIC
    dripcore
    stunning-waddle
    nlohmann_json
    isocline
    OpenSSL::SSL
    OpenSSL::Crypto
)

# -----------------------------------------------
# Main executable
# -----------------------------------------------

add_executable(mistral-cli
    src/main.cpp
)

target_compile_features(mistral-cli PRIVATE cxx_std_20)

target_link_libraries(mistral-cli PRIVATE
    mistral-core
)

# -----------------------------------------------
# Tests
# -----------------------------------------------

file(GLOB TEST_SOURCES "test/*.cpp")

add_executable(tests
    ${TEST_SOURCES}
)

target_compile_features(tests PRIVATE cxx_std_20)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test
)

target_link_libraries(tests PRIVATE
    mistral-core
)

include(CTest)
add_test(NAME run_tests COMMAND tests)
