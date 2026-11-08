#include "knowledge/hashing_embedder.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>

namespace mistral::knowledge
{
  namespace
  {
    std::uint64_t fnv1a_64(std::string_view data)
    {
      constexpr std::uint64_t fnv_offset_basis = 0xcbf29ce484222325ULL;
      constexpr std::uint64_t fnv_prime = 0x100000001b3ULL;

      std::uint64_t hash = fnv_offset_basis;
      for (const char c : data)
      {
        hash ^= static_cast<unsigned char>(c);
        hash *= fnv_prime;
      }
      return hash;
    }

    // Lowercased alphanumeric tokens of length >= 2.
    std::vector<std::string> tokenize(const std::string& text)
    {
      std::vector<std::string> tokens;
      std::string current;
      auto flush = [&]() {
        if (current.size() >= 2)
        {
          tokens.push_back(current);
        }
        current.clear();
      };
      for (const char c : text)
      {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
          current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else
        {
          flush();
        }
      }
      flush();
      return tokens;
    }
  } // namespace

  hashing_embedder::hashing_embedder(int dim) : dim_(dim > 0 ? dim : 384)
  {
  }

  std::string hashing_embedder::identity() const
  {
    return std::format("local-hash/{}", dim_);
  }

  dripcore::awaitable<std::vector<std::vector<float>>> hashing_embedder::embed_batch(
    std::vector<std::string> inputs,
    std::string& error
  )
  {
    std::vector<std::vector<float>> out;
    if (inputs.empty())
    {
      error = "embed_batch called with no inputs";
      co_return out;
    }

    out.reserve(inputs.size());
    for (const auto& text : inputs)
    {
      out.push_back(embed(text));
    }
    co_return out;
  }

  std::vector<float> hashing_embedder::embed(const std::string& text) const
  {
    std::vector<float> vec(static_cast<std::size_t>(dim_), 0.0f);

    const std::vector<std::string> tokens = tokenize(text);
    auto accumulate = [&](const std::string& feature) {
      const std::uint64_t hash = fnv1a_64(feature);
      const std::size_t index = static_cast<std::size_t>(hash % static_cast<std::uint64_t>(dim_));
      const float sign = (hash & 0x8000000000000000ULL) ? 1.0f : -1.0f;
      vec[index] += sign;
    };

    for (const auto& token : tokens)
    {
      accumulate(token);
    }
    // Word bigrams capture short phrases ("three way comparison").
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
      accumulate(tokens[i] + " " + tokens[i + 1]);
    }

    double norm = 0.0;
    for (const float v : vec)
    {
      norm += static_cast<double>(v) * v;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0)
    {
      const float inv = static_cast<float>(1.0 / norm);
      for (auto& v : vec)
      {
        v *= inv;
      }
    }
    return vec;
  }

} // namespace mistral::knowledge
