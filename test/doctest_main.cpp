#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <unistd.h>

#include <string>

int main(int argc, char** argv) {
  // Save the original stdout fd before any PTY test redirects fd 1.
  // cpptui writes via std::cout (fd 1), which PTY tests redirect to the
  // pseudo-terminal slave.  doctest also writes via std::cout by default,
  // so its failure output would be lost in the PTY buffer.
  //
  // We redirect doctest's output to /dev/fd/<orig_stdout> via --out.
  // doctest opens this as a std::fstream (separate from std::cout), so
  // doctest output always reaches the real terminal, while cpptui's
  // std::cout output still goes to fd 1 (the PTY slave during tests).
  int orig_stdout = ::dup(STDOUT_FILENO);

  doctest::Context ctx;
  ctx.applyCommandLine(argc, argv);
  if (orig_stdout >= 0) {
    std::string out_path = "/dev/fd/" + std::to_string(orig_stdout);
    ctx.setOption("out", out_path.c_str());
  }
  int res = ctx.run();

  if (orig_stdout >= 0) ::close(orig_stdout);
  return res;
}
