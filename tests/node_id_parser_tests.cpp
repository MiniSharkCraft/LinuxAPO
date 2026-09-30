#include "../src/pipewire/NodeIdParser.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
void expectInvalid(const char *label, const std::string &text) {
  if (skyapo::pipewire::parseNodeId(text)) {
    std::cerr << label << " NODE_ID unexpectedly parsed: '" << text << "'\n";
    std::exit(1);
  }
}
} // namespace

int main() {
  expectInvalid("empty", "");
  expectInvalid("junk", "12x");
  expectInvalid("whitespace", " 12");
  expectInvalid("overflow", "4294967296");

  const auto zero = skyapo::pipewire::parseNodeId("0");
  const auto max = skyapo::pipewire::parseNodeId(
      std::to_string(std::numeric_limits<uint32_t>::max()));
  const auto ordinary = skyapo::pipewire::parseNodeId("12345");
  if (!zero || *zero != 0 || !max ||
      *max != std::numeric_limits<uint32_t>::max() || !ordinary ||
      *ordinary != 12345) {
    std::cerr << "valid NODE_ID values did not parse correctly\n";
    return 1;
  }

  std::cout << "NODE_ID parser rejects empty/junk/overflow and accepts valid uint32 values\n";
}
