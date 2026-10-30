cmake_minimum_required(VERSION 3.16)
project(mistral-cli C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Feature options
# -----------------------------------------------------------------------------

# Tree-sitter code tools (outline, read_symbol, find_symbol, find_references,
# ast_search). When OFF, the binary is smaller and builds faster, but the
# syntax-aware code tools are unavailable — use XERJ (via MCP) for code
# search instead.
option(MISTRAL_ENABLE_TREE_SITTER "Build with tree-sitter code tools" ON)

# Live MCP integration tests. These talk to real servers - the Storybook dev
# server on 127.0.0.1:6006, a xerj node on localhost:9200, and godbolt.org
# over the internet - so they are opt-in and off by default. They skip
# themselves when the server is unreachable, but they are slow, assert on
# third-party tool surfaces, and cannot run in a sandboxed CI.
#
# Enable with: cmake -DMISTRAL_ENABLE_LIVE_MCP_TESTS=ON ..
option(MISTRAL_ENABLE_LIVE_MCP_TESTS "Build tests that require a live MCP server" OFF)

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
# Third-party: tree-sitter (parsing engine) + grammars (optional)
# -----------------------------------------------------------------------------

if(MISTRAL_ENABLE_TREE_SITTER)
  # tree-sitter and its grammars default to shared libraries; we link them
  # statically into mistral-core.
  set(BUILD_SHARED_LIBS OFF)

  add_subdirectory(third_party/tree-sitter)

  # Grammars are compiled from their checked-in generated sources. We do not use
  # the grammar's own CMakeLists because it can try to regenerate parser.c with
  # the tree-sitter CLI, which we do not want as a build dependency.
  add_library(tree-sitter-cpp STATIC
      third_party/grammars/tree-sitter-cpp/src/parser.c
      third_party/grammars/tree-sitter-cpp/src/scanner.c
  )
  target_include_directories(tree-sitter-cpp PRIVATE
      third_party/grammars/tree-sitter-cpp/src
  )
endif()

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
# Embedded workspace templates (generated at build time)
# -----------------------------------------------------------------------------

set(EMBEDDED_TEMPLATES_CPP
    ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_templates.cpp)

add_custom_command(
    OUTPUT ${EMBEDDED_TEMPLATES_CPP}
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${EMBEDDED_TEMPLATES_CPP}
            -DTEMPLATES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/templates
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_templates.cmake
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/sys_patchset_instructions.md
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/sys_prompt.md
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/sys_prompt_researcher.md
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/sys_prompt_xerj.md
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/policy.json
        ${CMAKE_CURRENT_SOURCE_DIR}/templates/mistral-cli.json
    COMMENT "Generating embedded workspace templates"
    VERBATIM
)

# -----------------------------------------------------------------------------
# Embedded tree-sitter queries (generated at build time, optional)
# -----------------------------------------------------------------------------

if(MISTRAL_ENABLE_TREE_SITTER)
  set(EMBEDDED_QUERIES_CPP
      ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_queries.cpp)

  add_custom_command(
      OUTPUT ${EMBEDDED_QUERIES_CPP}
      COMMAND ${CMAKE_COMMAND}
              -DOUT=${EMBEDDED_QUERIES_CPP}
              -DGRAMMARS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/grammars
              -DREPO_QUERIES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/queries
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_queries.cmake
      DEPENDS
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_queries.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/grammars/tree-sitter-cpp/queries/tags.scm
          ${CMAKE_CURRENT_SOURCE_DIR}/queries/cpp-references.scm
      COMMENT "Generating embedded tree-sitter queries"
      VERBATIM
  )
else()
  # Stub: no queries when tree-sitter is disabled. The declarations in
  # embedded_resources.h are guarded by MISTRAL_ENABLE_TREE_SITTER.
  set(EMBEDDED_QUERIES_CPP "")
endif()

# -----------------------------------------------------------------------------
# Core library
# -----------------------------------------------------------------------------

set(MISTRAL_CORE_SOURCES
    lib/http/tls_connection.cpp
    lib/mcp/mcp_client.cpp
    lib/mcp/mcp_stdio_client.cpp
    lib/mcp/mcp_manager.cpp
    lib/mcp/mcp_auto_connect_task.cpp
    lib/api/provider_base.cpp
    lib/api/providers/mistral_provider.cpp
    lib/api/providers/openai_provider.cpp
    lib/api/provider_registry.cpp
    lib/template/template_engine.cpp
    lib/tools/file_tools.cpp
    lib/tools/note_tools.cpp
    lib/tools/exec_tools.cpp
    lib/tools/task_tools.cpp
    lib/tools/research_tools.cpp
    lib/tools/ask_tools.cpp
    lib/console/console_task.cpp
    lib/slash_command/slash_command_registry.cpp
    lib/slash_command/slash_command_executor_task.cpp
    lib/agent/agent_task_base.cpp
    lib/agent/subagent_task.cpp
    lib/agent/server_agent_task.cpp
    lib/agent/policy_engine.cpp
    lib/agent/approval_manager.cpp
    lib/agent/approval_prompt.cpp
    lib/agent/approval_renderers.cpp
    lib/agent/agent_system.cpp
    lib/agent/agent_system_task.cpp
    lib/agent/agent_system_stats_task.cpp
    lib/tools/edit_parser.cpp
    lib/tools/edit_validator.cpp
    lib/agent/save_command.cpp
    lib/agent/load_command.cpp
    lib/agent/usage_command.cpp
    lib/agent/tool_command.cpp
    lib/agent/compact_command.cpp
    lib/agent/mcp_command.cpp
    lib/tools/patchset_tools.cpp
    lib/common/string_utils.cpp
    lib/common/executable_finder.cpp
    lib/common/subprocess.cpp
    lib/trace/trace.cpp
    lib/agent/workspace_bootstrap.cpp
    ${EMBEDDED_TEMPLATES_CPP}
)

if(MISTRAL_ENABLE_TREE_SITTER)
    list(APPEND MISTRAL_CORE_SOURCES
        lib/tools/code_index.cpp
        lib/tools/code_tools.cpp
        ${EMBEDDED_QUERIES_CPP}
    )
endif()

add_library(mistral-core STATIC
    ${MISTRAL_CORE_SOURCES}
)

target_compile_features(mistral-core PUBLIC cxx_std_20)

target_include_directories(mistral-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/common
)

set(MISTRAL_CORE_LIBS
    dripcore
    stunning-waddle
    just-a-uri
    nlohmann_json
    isocline
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(MISTRAL_ENABLE_TREE_SITTER)
    list(APPEND MISTRAL_CORE_LIBS tree-sitter tree-sitter-cpp)
    target_compile_definitions(mistral-core PUBLIC MISTRAL_ENABLE_TREE_SITTER=1)
endif()

target_link_libraries(mistral-core PUBLIC
    ${MISTRAL_CORE_LIBS}
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
# MCP connect simulator (debug tool — simulates /mcp connect without TUI)
# -----------------------------------------------------------------------------

add_executable(mcp-connect-sim
    src/mcp_connect_sim.cpp
)

target_compile_features(mcp-connect-sim PRIVATE cxx_std_20)

target_link_libraries(mcp-connect-sim PRIVATE
    mistral-core
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")

# Exclude tree-sitter-dependent tests when the feature is disabled.
if(NOT MISTRAL_ENABLE_TREE_SITTER)
    list(REMOVE_ITEM TEST_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test/code_tools_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/test/tree_sitter_test.cpp
    )
endif()

add_executable(tests
    ${TEST_SOURCES}
)

target_compile_features(tests PRIVATE cxx_std_20)

# Live MCP tests are guarded by this define in the test sources.
if(MISTRAL_ENABLE_LIVE_MCP_TESTS)
    target_compile_definitions(tests PRIVATE MISTRAL_ENABLE_LIVE_MCP_TESTS=1)
endif()

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

# -----------------------------------------------------------------------------
# content_log_demo: standalone demo of ContentLog widget loading a chat log
# -----------------------------------------------------------------------------

add_executable(content_log_demo
    src/content_log_demo.cpp
)

target_compile_features(content_log_demo PRIVATE cxx_std_20)

target_link_libraries(content_log_demo PRIVATE
    mistral-core
    cpptui::cpptui
)

include(CTest)
add_test(NAME run_tests COMMAND tests)
