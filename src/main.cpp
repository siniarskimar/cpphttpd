#include <fmt/core.h>
#include <cstdio>
#include <fmt/std.h>

int main(const int argc, const char* argv[]) {
  fmt::print(stderr, "log: {} world\n", "hello");
  return 0;
}
