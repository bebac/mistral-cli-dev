#include <doctest/doctest.h>

#include "tree_sitter/api.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// Declared in the grammar's bindings (tree-sitter-cpp.h). Declared inline to
// avoid adding a grammar-specific include path to the test target.
extern "C" {
const TSLanguage* tree_sitter_cpp(void);
}

TEST_CASE("tree-sitter core and cpp grammar integrate and parse")
{
  TSParser* parser = ts_parser_new();
  REQUIRE(ts_parser_set_language(parser, tree_sitter_cpp()));

  const char* source =
    "namespace mistral {\n"
    "class tool {\n"
    "public:\n"
    "  int run(int n);\n"
    "};\n"
    "}\n";

  TSTree* tree = ts_parser_parse_string(
    parser, nullptr, source, static_cast<uint32_t>(std::strlen(source))
  );
  REQUIRE(tree != nullptr);

  TSNode root = ts_tree_root_node(tree);
  CHECK(std::string(ts_node_type(root)) == "translation_unit");

  // The namespace is the first named top-level construct.
  TSNode ns = ts_node_named_child(root, 0);
  CHECK(std::string(ts_node_type(ns)) == "namespace_definition");

  // The namespace's first named child is its identifier...
  TSNode ns_name = ts_node_named_child(ns, 0);
  CHECK(std::string(ts_node_type(ns_name)) == "namespace_identifier");

  // ... followed by the namespace body, which contains the class definition.
  TSNode ns_body = ts_node_named_child(ns, 1);
  CHECK(std::string(ts_node_type(ns_body)) == "declaration_list");

  TSNode class_node = ts_node_named_child(ns_body, 0);
  CHECK(std::string(ts_node_type(class_node)) == "class_specifier");

  // A clean parse (no ERROR nodes) proves the grammar's ABI version is
  // compatible with the tree-sitter core we build against.
  CHECK_FALSE(ts_node_has_error(root));

  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

TEST_CASE("tree-sitter queries extract symbol definitions like tags.scm")
{
  // Mirrors the definition patterns from
  // third_party/grammars/tree-sitter-cpp/queries/tags.scm.
  const char* tags_query =
    "(class_specifier name: (type_identifier) @name) @definition.class\n"
    "(function_declarator declarator: (identifier) @name) @definition.function\n"
    "(function_declarator declarator: (field_identifier) @name) @definition.function\n"
    "(type_definition declarator: (type_identifier) @name) @definition.type\n"
    "(enum_specifier name: (type_identifier) @name) @definition.type\n";

  const char* source =
    "class tool_registry {\n"
    "public:\n"
    "  void register_tool(const char* name);\n"
    "};\n"
    "typedef int handle;\n"
    "enum class color { red };\n"
    "int main() { return 0; }\n";

  TSParser* parser = ts_parser_new();
  REQUIRE(ts_parser_set_language(parser, tree_sitter_cpp()));

  TSTree* tree = ts_parser_parse_string(
    parser, nullptr, source, static_cast<uint32_t>(std::strlen(source))
  );
  REQUIRE(tree != nullptr);

  // The query must compile against the grammar.
  uint32_t error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  TSQuery* query = ts_query_new(
    tree_sitter_cpp(), tags_query,
    static_cast<uint32_t>(std::strlen(tags_query)), &error_offset, &error_type
  );
  REQUIRE(query != nullptr);
  REQUIRE(error_type == TSQueryErrorNone);

  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));

  struct symbol { std::string kind; std::string name; };
  std::vector<symbol> symbols;

  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    symbol s;
    for (uint32_t i = 0; i < match.capture_count; ++i) {
      uint32_t len = 0;
      const char* capture = ts_query_capture_name_for_id(
        query, match.captures[i].index, &len
      );
      std::string capture_name(capture, len);
      const TSNode node = match.captures[i].node;
      if (capture_name == "name") {
        s.name = std::string(
          source + ts_node_start_byte(node),
          ts_node_end_byte(node) - ts_node_start_byte(node)
        );
      } else if (capture_name.rfind("definition.", 0) == 0) {
        s.kind = capture_name.substr(std::strlen("definition."));
      }
    }
    if (!s.name.empty()) {
      symbols.push_back(s);
    }
  }

  std::sort(symbols.begin(), symbols.end(),
    [](const symbol& a, const symbol& b) {
      return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });

  REQUIRE(symbols.size() == 5);
  CHECK(symbols[0].kind == "class");
  CHECK(symbols[0].name == "tool_registry");
  CHECK(symbols[1].kind == "function");
  CHECK(symbols[1].name == "main");
  CHECK(symbols[2].kind == "function");
  CHECK(symbols[2].name == "register_tool");
  CHECK(symbols[3].kind == "type");
  CHECK(symbols[3].name == "color");
  CHECK(symbols[4].kind == "type");
  CHECK(symbols[4].name == "handle");

  ts_query_cursor_delete(cursor);
  ts_query_delete(query);
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}