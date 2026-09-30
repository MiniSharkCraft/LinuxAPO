#pragma once

#include <string>
#include <string_view>

namespace skyapo::core {

// Matches EAPO Device: patterns using the upstream AND/OR and GUID rules.
bool devicePatternMatches(std::wstring_view deviceString,
                          std::wstring_view pattern);

} // namespace skyapo::core
