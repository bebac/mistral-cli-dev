#ifndef MISTRAL_HASHING_EMBEDDER_H
#define MISTRAL_HASHING_EMBEDDER_H

#include "knowledge/embedder.h"

#include <string>
#include <vector>

namespace mistral::knowledge
{
  // Deterministic, offline text embedder based on the hashing trick:
  // word unigrams and bigrams are hashed into a fixed-dimension vector
  // with signed feature hashing, then L2-normalized. No model, no
  // network. Good enough for lexical similarity over documentation and
  // code; mistral_embedder replaces it behind the same interface when a
  // remote provider is configured.
  class hashing_embedder : public embedder
  {
  public:
    explicit hashing_embedder(int dim = 384);

    int dim() const override
    {
      return dim_;
    }

    std::string identity() const override;

    int batch_size() const override
    {
      return 256;
    }

    // Embed text into an L2-normalized vector of dim() floats. Empty
    // or token-less input yields the zero vector.
    //
    // Synchronous and single-text: knowledge_store still calls this
    // directly. It goes away when the store moves to embed_batch().
    std::vector<float> embed(const std::string& text) const;

    dripcore::awaitable<std::vector<std::vector<float>>>
      embed_batch(std::vector<std::string> inputs, std::string& error) override;

  private:
    int dim_;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_HASHING_EMBEDDER_H
