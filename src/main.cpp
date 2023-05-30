#include <fmt/core.h>
#include <cstdio>
#include <fmt/std.h>
#include <string_view>

#include <boost/asio.hpp>


int main(const int argc, const char* argv[]) {
    fmt::print(stderr, "Hello {}\n", "World");
}
