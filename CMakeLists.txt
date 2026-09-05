cmake_minimum_required(VERSION 3.16)
project(mistral-cli CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Third-party: dripcore
# -----------------------------------------------------------------------------

add_subdirectory(third_party/dripcore)

# -----------------------------------------------------------------------------
# Third-party: stunning-waddle (HTTP/2 library + URI library)
# -----------------------------------------------------------------------------

add_subdirectory(third_party/stunning-waddle)
add_subdirectory(third_party/just-a-uri)

# -----------------------------------------------------------------------------
# Third-party: isocline (readline library)
# -----------------------------------------------------------------------------

add_subdirectory(third_party/isocline)

# -----------------------------------------------------------------------------
# Third-party: OpenSSL
# -----------------------------------------------------------------------------

find_package(OpenSSL REQUIRED)

# -----------------------------------------------------------------------------
# Third-party: nlohmann/json (header-only)
# -----------------------------------------------------------------------------

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# -----------------------------------------------------------------------------
# Core library
# -----------------------------------------------------------------------------

add_library(mistral-core STATIC
    lib/http/tls_connection.cpp
    lib/api/mistral_api.cpp
    lib/template/template_engine.cpp
    lib/tools/file_tools.cpp
    lib/tools/note_tools.cpp
    lib/tools/grep_tools.cpp
    lib/tools/exec_tools.cpp
    lib/tools/task_tools.cpp
    lib/console/console_task.cpp
    lib/slash_command/slash_command_registry.cpp
    lib/slash_command/slash_command_executor_task.cpp
    lib/agent/agent_task_base.cpp
    lib/agent/subagent_task.cpp
    lib/agent/policy_engine.cpp
    lib/agent/approval_manager.cpp
    lib/agent/approval_prompt.cpp
    lib/agent/agent_system.cpp
    lib/agent/agent_system_task.cpp
    lib/agent/agent_system_stats_task.cpp
    lib/agent/edit_parser.cpp
    lib/agent/edit_validator.cpp
    lib/agent/save_command.cpp
    lib/agent/load_command.cpp
    lib/agent/usage_command.cpp
    lib/agent/patchset_verify_command.cpp
    lib/agent/tool_command.cpp
    lib/tools/patchset_tools.cpp
    lib/common/string_utils.cpp
    lib/trace/trace.cpp
)

target_compile_features(mistral-core PUBLIC cxx_std_20)

target_include_directories(mistral-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/common
)

target_link_libraries(mistral-core PUBLIC
    dripcore
    stunning-waddle
    just-a-uri
    nlohmann_json
    isocline
    OpenSSL::SSL
    OpenSSL::Crypto
)

# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------

add_executable(mistral-cli
    src/main.cpp
)

target_compile_features(mistral-cli PRIVATE cxx_std_20)

target_link_libraries(mistral-cli PRIVATE
    mistral-core
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")

add_executable(tests
    ${TEST_SOURCES}
)

target_compile_features(tests PRIVATE cxx_std_20)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test
)

target_link_libraries(tests PRIVATE
    mistral-core
    cpptui::cpptui
)

# -----------------------------------------------------------------------------
# Third-party: cpp-tui (TUI library)
# ----------------------------------------------------------------=============

add_subdirectory(third_party/cpp-tui)

# -----------------------------------------------------------------------------
# TUI executable
# ----------------------------------------------------------------=============

add_executable(mistral-tui
    src/tui_main.cpp
)

target_compile_features(mistral-tui PRIVATE cxx_std_20)

target_link_libraries(mistral-tui PRIVATE
    mistral-core
    cpptui::cpptui
)

include(CTest)
add_test(NAME run_tests COMMAND tests)
