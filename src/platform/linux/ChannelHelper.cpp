#include "helpers/ChannelHelper.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <unordered_map>

std::unordered_map<std::wstring, int> ChannelHelper::channelNameToPosMap;
std::unordered_map<int, std::wstring> ChannelHelper::channelPosToNameMap;
ChannelHelper ChannelHelper::instance;

ChannelHelper::ChannelHelper() {
  channelNameToPosMap = {{L"L", 0x1},    {L"R", 0x2},    {L"C", 0x4},
                         {L"LFE", 0x8},  {L"RL", 0x10},  {L"RR", 0x20},
                         {L"RC", 0x100}, {L"SL", 0x200}, {L"SR", 0x400}};
  for (const auto &entry : channelNameToPosMap)
    channelPosToNameMap[entry.second] = entry.first;
}

int ChannelHelper::getDefaultChannelMask(int channelCount) {
  switch (channelCount) {
  case 1:
    return 0x4;
  case 2:
    return 0x3;
  case 4:
    return 0x33;
  case 6:
    return 0x3f;
  case 8:
    return 0x63f;
  default:
    return 0;
  }
}

std::vector<std::wstring> ChannelHelper::getChannelNames(int channelCount,
                                                         int channelMask) {
  std::vector<std::wstring> names;
  for (int bit = 0; bit < 31; ++bit) {
    const int position = 1 << bit;
    if (!(channelMask & position))
      continue;
    auto it = channelPosToNameMap.find(position);
    names.push_back(it == channelPosToNameMap.end()
                        ? std::to_wstring(names.size() + 1)
                        : it->second);
  }
  while (names.size() < static_cast<size_t>(channelCount))
    names.push_back(std::to_wstring(names.size() + 1));
  return names;
}

int ChannelHelper::getChannelIndex(std::wstring word,
                                   const std::vector<std::wstring> &names,
                                   bool allowNew) {
  if (word.empty())
    return -1;
  if (std::iswdigit(word.front())) {
    wchar_t *end = nullptr;
    const long index = std::wcstol(word.c_str(), &end, 10) - 1;
    if (*end || index < 0 || index >= static_cast<long>(names.size()))
      return -1;
    return static_cast<int>(index);
  }
  auto find = [&](const std::wstring &name) {
    return std::find(names.begin(), names.end(), name);
  };
  auto it = find(word);
  if (it == names.end()) {
    const std::wstring aliases[][2] = {{L"SL", L"RL"},
                                       {L"SR", L"RR"},
                                       {L"RL", L"SL"},
                                       {L"RR", L"SR"},
                                       {L"SUB", L"LFE"}};
    for (const auto &alias : aliases)
      if (word == alias[0]) {
        it = find(alias[1]);
        if (it != names.end())
          break;
      }
  }
  if (it != names.end())
    return static_cast<int>(it - names.begin());
  (void)allowNew;
  return -1;
}
