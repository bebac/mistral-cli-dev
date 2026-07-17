#ifndef MISTRAL_TOOL_REGISTRY_H
#define MISTRAL_TOOL_REGISTRY_H

#include <nlohmann/json.hpp>

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

    template <typename T>
    json build_type_schema()
    {
      using U = std::decay_t<T>;

      if constexpr (is_vector<U>::value) {
        using V = typename is_vector<U>::inner_type;
        return {{"type", "array"}, {"items", build_type_schema<V>()}};
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

  // ---- Abstract tool interface ----

  class tool_interface
  {
  public:
    virtual ~tool_interface() = default;
    virtual json execute(const json& args) = 0;
    virtual json get_schema(const std::string& tool_name) = 0;
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

    json execute(const json& j_args) override
    {
      return call_impl(j_args, std::index_sequence_for<Args...>{});
    }

    json get_schema(const std::string& tool_name) override
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
          } else {
            required_list.push_back(info.name);
          }
          properties[info.name] = property;
        }.template operator()<Args>(),
        ...);
      // Mistral API format: {"type":"function","function":{...}}
      return {
        {"type", "function"},
        {"function", {
          {"name", tool_name},
          {"description", description_},
          {"parameters", {
            {"type", "object"},
            {"properties", properties},
            {"required", required_list}
          }}
        }}
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
    template <typename T>
    static T from_json_arg(const json& j, const arg_info& info)
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

    template <size_t... Is>
    Ret call_impl(const json& j_args, std::index_sequence<Is...>)
    {
      return func_(from_json_arg<Args>(j_args, args_info_[Is])...);
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

    json dispatch(const json& tool_call)
    {
      auto name = tool_call["function"]["name"].get<std::string>();
      auto args = json::parse(tool_call["function"]["arguments"].get<std::string>());

      auto it = tools_.find(name);
      if (it != tools_.end()) {
        try {
          return it->second->execute(args);
        } catch (const std::exception& e) {
          return {{"error", e.what()}};
        }
      }
      return {{"error", std::format("tool '{}' not found", name)}};
    }

    // Get tool definitions array for the API request.
    json get_schema() const
    {
      json schema = json::array();
      for (const auto& [name, tool] : tools_) {
        schema.push_back(tool->get_schema(name));
      }
      return schema;
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
  };

} // namespace mistral::agent

#endif // MISTRAL_TOOL_REGISTRY_H
