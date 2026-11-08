#ifndef MISTRAL_KNOWLEDGE_EMBEDDER_H
#define MISTRAL_KNOWLEDGE_EMBEDDER_H

#include <dripcore/awaitable.h>

#include <string>
#include <vector>

namespace mistral::knowledge
{
  // Turns text into embedding vectors. Implementations differ in whether
  // they are local (hashing_embedder, no network) or remote
  // (mistral_embedder, codestral-embed over the shared HTTP transport),
  // but the index only ever sees this interface.
  //
  // identity() names the vector space. Two vectors are comparable only
  // when their identities match, so the store records the identity in
  // its metadata and rebuilds the index when it changes.
  class embedder
  {
  public:
    virtual ~embedder() = default;

    embedder(const embedder&) = delete;
    embedder& operator=(const embedder&) = delete;

    // Dimension of every vector returned by embed_batch().
    virtual int dim() const = 0;

    // Stable identifier of the vector space: provider, model, dimension,
    // dtype and chunker version. Stored in the index metadata.
    virtual std::string identity() const = 0;

    // Number of inputs the store should pass per embed_batch() call.
    // Keeps request bodies and the blast radius of a failure small; a
    // local embedder can afford larger batches than a network one.
    virtual int batch_size() const = 0;

    // Total characters one request may carry, or 0 when the provider has
    // no such limit (a local embedder). The store groups chunks across
    // file boundaries into requests of this size, so a one-chunk file
    // does not cost a whole round trip.
    virtual int max_batch_chars() const
    {
      return 0;
    }

    // Embed a batch of inputs, returning one vector per input in input
    // order. On failure returns an empty vector and sets error.
    //
    // inputs is taken by value on purpose: a coroutine's reference
    // parameters dangle when callers pass temporaries, because the body
    // runs at the first co_await, after the temporary is gone.
    //
    // Callers must not pass an empty batch, and must not exceed the
    // provider's hard limit (see each implementation); both are rejected
    // with an error rather than silently truncated.
    virtual dripcore::awaitable<std::vector<std::vector<float>>>
      embed_batch(std::vector<std::string> inputs, std::string& error) = 0;

  protected:
    embedder() = default;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_KNOWLEDGE_EMBEDDER_H
