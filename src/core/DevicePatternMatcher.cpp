/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2014  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "DevicePatternMatcher.h"

#include "helpers/StringHelper.h"

#include <regex>
#include <string>
#include <vector>

namespace skyapo::core {

// Port of Equalizer APO's DeviceFilterFactory::matchDevice(). A semicolon is
// OR between groups; words within a group are AND. Keep this aligned with the
// upstream implementation at filters/DeviceFilterFactory.cpp.
bool devicePatternMatches(std::wstring_view deviceString,
                          std::wstring_view pattern) {
  const std::wstring value = StringHelper::trim(std::wstring(pattern)) + L";";
  std::vector<std::vector<std::wstring>> groups;
  std::vector<std::wstring> words;
  std::wstring current;

  for (const wchar_t character : value) {
    if (character == L' ' || character == L';') {
      if (!current.empty()) {
        words.push_back(std::move(current));
        current.clear();
      }
      if (character == L';' && !words.empty()) {
        groups.push_back(words);
        words.clear();
      }
    } else {
      current += character;
    }
  }

  static const std::wregex guidPattern(
      L"\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
      L"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\}");
  const std::wstring withoutGuid =
      std::regex_replace(std::wstring(deviceString), guidPattern, L"");
  const std::wstring foldedDevice =
      StringHelper::toLowerCase(std::wstring(deviceString));
  const std::wstring foldedWithoutGuid = StringHelper::toLowerCase(withoutGuid);

  for (const auto &group : groups) {
    if (group.size() == 1 &&
        StringHelper::toLowerCase(group.front()) == L"all")
      return true;

    bool groupMatches = true;
    for (const auto &word : group) {
      const auto foldedWord = StringHelper::toLowerCase(word);
      const auto &matchText = word.find(L'{') == std::wstring::npos
                                  ? foldedWithoutGuid
                                  : foldedDevice;
      if (matchText.find(foldedWord) == std::wstring::npos) {
        groupMatches = false;
        break;
      }
    }
    if (groupMatches)
      return true;
  }
  return false;
}

} // namespace skyapo::core
