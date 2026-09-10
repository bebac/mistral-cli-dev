#ifndef MISTRAL_TOOL_REGISTRY_H
#define MISTRAL_TOOL_REGISTRY_H

#include <nlohmann/json.hpp>
#include <dripcore/awaitable.h>

#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  // ---- Argument metadata ----

  struct arg_info
  {
    std::string name;
    std::string description;
    std::optional<std::string> default_value;  // JSON string
  };

  inline arg_info arg(std::string name, std::string desc)
  {
    return {std::move(name), std::move(desc), std::nullopt};
  }

  template <typename T>
  inline arg_info arg(std::string name, std::string desc, const T& default_val)
  {
    json j = default_val;
    return {std::move(name), std::move(desc), j.dump()};
  }

  // ---- Type-to-JSON-schema helper ----

  namespace detail
  {
    template <typename T> struct is_vector : std::false_type {};
    template <typename T, typename A> struct is_vector<std::vector<T, A>> : std::true_type {
      using inner_type = T;
    };

    template <typename T> struct is_optional : std::false_type {};
    template <typename T> struct is_optional<std::optional<T>> : std::true_type {
      using inner_type = T;
    };

    template <typename T>
    json build_type_schema()
    {
      using U = std::decay_t<T>;

      if constexpr (is_vector<U>::value) {
        using V = typename is_vector<U>::inner_type;
        return {{"type", "array"}, {"items", build_type_schema<V>()}};
      }
      else if constexpr (is_optional<U>::value) {
        using V = typename is_optional<U>::inner_type;
        // For optional types, we don't mark as required and use the inner type's schema
        return build_type_schema<V>();
      }
      else if constexpr (std::is_same_v<U, bool>) {
        return {{"type", "boolean"}};
      }
      else if constexpr (std::is_integral_v<U>) {
        return {{"type", "integer"}};
      }
      else if constexpr (std::is_floating_point_v<U>) {
        return {{"type", "number"}};
      }
      else if constexpr (std::is_same_v<U, std::string>) {
        return {{"type", "string"}};
      }
      else {
        return {{"type", "string"}};
      }
    }
  } // namespace detail

  // ---- Helper to detect awaitable types ----
  template <typename T>
  struct is_awaitable : std::false_type {};

  template <typename T>
  struct is_awaitable<dripcore::awaitable<T>> : std::true_type {};

  template <typename T>
  constexpr bool is_awaitable_v = is_awaitable<T>::value;

  // ---- Abstract tool interface ----

  class tool_interface
  {
  public:
    virtual ~tool_interface() = default;
    virtual dripcore::awaitable<json> execute(const json& args) = 0;
    virtual json get_schema(const std::string& tool_name, bool strict = false) = 0;
    virtual std::shared_ptr<tool_interface> clone() const = 0;
  };

  // ---- Concrete tool wrapper ----

  template <typename Ret, typename... Args>
  class tool_wrapper : public tool_interface
  {
  public:
    tool_wrapper(std::function<Ret(Args...)> f, std::vector<arg_info> info, std::string desc)
      : func_(std::move(f)), args_info_(std::move(info)), description_(std::move(desc))
    {}

    dripcore::awaitable<json> execute(const json& j_args) override
    {
      auto result = call_impl(j_args, std::index_sequence_for<Args...>{});

      if constexpr (std::is_same_v<Ret, json>) {
        co_return result;
      } else if constexpr (is_awaitable_v<Ret>) {
        co_return json(co_await result);
      } else {
        co_return json(result);
      }
    }

    json get_schema(const std::string& tool_name, bool strict = false) override
    {
      json properties = json::object();
      json required_list = json::array();
      size_t i = 0;

      (
        [&]<typename T>() {
          const auto& info = args_info_[i++];
          json property = detail::build_type_schema<T>();
          property["description"] = info.description;

          if (info.default_value) {
            property["default"] = json::parse(*info.default_value);
          }
          // In strict mode all properties must be listed in "required",
          // even those with default values.
          if (!info.default_value || strict) {
            required_list.push_back(info.name);
          }
          properties[info.name] = property;
        }.template operator()<Args>(),
        ...);

      json parameters = {
        {"type", "object"},
        {"properties", properties},
        {"required", required_list}
      };
      if (strict) {
        parameters["additionalProperties"] = false;
      }

      json function = {
        {"name", tool_name},
        {"description", description_},
        {"parameters", parameters}
      };
      if (strict) {
        function["strict"] = true;
      }

      // Mistral API format: {"type":"function","function":{...}}
      return {
        {"type", "function"},
        {"function", function}
      };
    }

    std::shared_ptr<tool_interface> clone() const override
    {
      // Create a new wrapper with the same function and metadata
      // Note: The captured variables in func_ (like tool instance pointers) are preserved
      return std::make_shared<tool_wrapper<Ret, Args...>>(
        func_, args_info_, description_
      );
    }

  private:
    // Helper class to avoid template ambiguity with std::optional
    template <typename T, typename = void>
    struct from_json_helper
    {
      static T get(const json& j, const arg_info& info)
      {
        if (!j.contains(info.name) && info.default_value) {
          return json::parse(*info.default_value).get<T>();
        }
        if (!j.contains(info.name)) {
          throw std::runtime_error("missing required argument: " + info.name);
        }
        if constexpr (std::is_same_v<T, bool>) {
          const auto& v = j[info.name];
          if (v.is_boolean()) return v.get<bool>();
          if (v.is_number_integer()) return v.get<int>() != 0;
          if (v.is_string()) {
            auto s = v.get<std::string>();
            return s == "true" || s == "1";
          }
          throw std::runtime_error("expected boolean for " + info.name);
        } else {
          return j[info.name].get<T>();
        }
      }
    };

    // Partial specialization for std::optional<T>
    template <typename T>
    struct from_json_helper<std::optional<T>>
    {
      static std::optional<T> get(const json& j, const arg_info& info)
      {
        if (!j.contains(info.name)) {
          // If not provided and has default value, parse it
          if (info.default_value) {
            try {
              auto parsed = json::parse(*info.default_value);
              if (parsed.is_null()) {
                return std::nullopt;
              }
              return parsed.get<T>();
            } catch (...) {
              return std::nullopt;
            }
          }
          // Not provided and no default - return nullopt
          return std::nullopt;
        }

        // Provided in JSON
        const auto& v = j[info.name];
        if (v.is_null()) {
          return std::nullopt;
        }
        return v.get<T>();
      }
    };

    template <typename T>
    static T from_json_arg(const json& j, const arg_info& info)
    {
      return from_json_helper<T>::get(j, info);
    }


    // Get a human-readable name for a C++ type
    template <typename T>
    static std::string type_name()
    {
      using U = std::decay_t<T>;
      if constexpr (detail::is_vector<U>::value) {
        return "array of " + type_name<typename detail::is_vector<U>::inner_type>();
      } else if constexpr (detail::is_optional<U>::value) {
        return type_name<typename detail::is_optional<U>::inner_type>();
      } else if constexpr (std::is_same_v<U, bool>) {
        return "boolean";
      } else if constexpr (std::is_integral_v<U>) {
        return "integer";
      } else if constexpr (std::is_floating_point_v<U>) {
        return "number";
      } else if constexpr (std::is_same_v<U, std::string>) {
        return "string";
      } else {
        return "string";
      }
    }

    // Get a human-readable name for a JSON value's type
    static std::string json_type_name(const json& v)
    {
      if (v.is_string()) return "string";
      if (v.is_array()) return "array";
      if (v.is_object()) return "object";
      if (v.is_number_integer()) return "integer";
      if (v.is_number_float()) return "number";
      if (v.is_boolean()) return "boolean";
      if (v.is_null()) return "null";
      return "unknown";
    }

    // Wraps from_json_arg with better error messages on type errors.
    // Raw nlohmann json exceptions (e.g. "[json.exception.type_error.302]...")
    // are replaced with actionable messages that include the argument name,
    // expected type, and actual value.
    template <typename T>
    static T safe_from_json_arg(const json& j, const arg_info& info)
    {
      try {
        return from_json_arg<T>(j, info);
      } catch (const std::exception& e) {
        std::string msg = e.what();
        // Only enhance raw nlohmann json exceptions; rethrow others
        // (e.g., "missing required argument: foo") as-is
        if (msg.find("[json.") == std::string::npos) {
          throw;
        }

        std::string actual = j.contains(info.name)
          ? json_type_name(j[info.name])
          : "missing";

        std::string value_str;
        if (j.contains(info.name)) {
          value_str = j[info.name].dump();
          if (value_str.size() > 100) value_str = value_str.substr(0, 100) + "...";
        }

        throw std::runtime_error(std::format(
          "Argument '{}' has wrong type: expected {}, but received {}{}{}",
          info.name,
          type_name<T>(),
          actual,
          value_str.empty() ? "" : " (value: ",
          value_str.empty() ? "" : value_str + ")"
        ));
      }
    }

    template <size_t... Is>
    Ret call_impl(const json& j_args, std::index_sequence<Is...>)
    {
      return func_(safe_from_json_arg<Args>(j_args, args_info_[Is])...);
    }

    std::function<Ret(Args...)> func_;
    std::vector<arg_info> args_info_;
    std::string description_;
  };

  // ---- Tool registry ----

  class tool_registry
  {
  public:
    // Default constructor
    tool_registry() = default;

    // Copy constructor - creates a deep copy of the registry
    tool_registry(const tool_registry& other)
      : strict_mode_(other.strict_mode_)
    {
      for (const auto& [name, tool] : other.tools_) {
        // Clone the tool_wrapper by re-registering with the same parameters
        // Note: This assumes the tool instances (T*) are still valid
        // The actual tool execution will use the same underlying instances
        tools_[name] = tool->clone();
      }
    }

    // Copy assignment operator
    tool_registry& operator=(const tool_registry& other)
    {
      if (this != &other) {
        tools_.clear();
        strict_mode_ = other.strict_mode_;
        for (const auto& [name, tool] : other.tools_) {
          tools_[name] = tool->clone();
        }
      }
      return *this;
    }

    template <typename T, typename Ret, typename... Args, typename... Names>
      requires (sizeof...(Args) == sizeof...(Names))
    void register_tool(std::string name, std::string desc, T* inst, Ret (T::*m)(Args...), Names... names)
    {
      std::vector<arg_info> info_vec = {names...};
      auto wrapper = std::make_shared<tool_wrapper<Ret, Args...>>(
        [inst, m](Args... args) { return (inst->*m)(args...); },
        std::move(info_vec), std::move(desc)
      );
      tools_[name] = std::move(wrapper);
    }

    dripcore::awaitable<json> dispatch(const json& tool_call)
    {
      auto name = tool_call["function"]["name"].get<std::string>();

      json args;
      try {
        args = json::parse(tool_call["function"]["arguments"].get<std::string>());
      } catch (const std::exception& e) {
        co_return json(std::string("Error: failed to parse tool arguments: ") + e.what());
      }

      auto it = tools_.find(name);
      if (it != tools_.end()) {
        try {
          co_return co_await it->second->execute(args);
        } catch (const std::exception& e) {
          co_return json(std::string("Error: ") + e.what());
        }
      }
      auto display = name.size() > 80 ? name.substr(0, 77) + "..." : name;
      co_return json(std::format("Error: tool '{}' not found", display));
    }

    // Get tool definitions array for the API request.
    json get_schema() const
    {
      json schema = json::array();
      for (const auto& [name, tool] : tools_) {
        schema.push_back(tool->get_schema(name, strict_mode_));
      }
      return schema;
    }

    // Enable/disable strict mode for tool schema generation.
    // When enabled, tool schemas include "strict": true,
    // "additionalProperties": false, and list all properties
    // (including optional ones) in "required".
    void set_strict_mode(bool enabled)
    {
      strict_mode_ = enabled;
    }

    // Check whether strict mode is enabled
    bool strict_mode() const
    {
      return strict_mode_;
    }

    // Check if a tool is registered
    bool has_tool(const std::string& name) const
    {
      return tools_.find(name) != tools_.end();
    }

    // Get a tool by name (returns nullptr if not found)
    std::shared_ptr<tool_interface> get_tool(const std::string& name) const
    {
      auto it = tools_.find(name);
      return it != tools_.end() ? it->second : nullptr;
    }

    // Get the underlying map (for iteration)
    const std::map<std::string, std::shared_ptr<tool_interface>>& get_tools_map() const
    {
      return tools_;
    }

    // Add a tool directly (for use by agent_system when filtering)
    void add_tool(const std::string& name, std::shared_ptr<tool_interface> tool)
    {
      tools_[name] = std::move(tool);
    }

  private:
    std::map<std::string, std::shared_ptr<tool_interface>> tools_;
    bool strict_mode_ = false;
  };

} // namespace mistral::agent

#endif // MISTRAL_TOOL_REGISTRY_H
