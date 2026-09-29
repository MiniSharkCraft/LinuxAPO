#pragma once
#include <stdexcept>
#include <string>
inline std::wstring eapoChannel(const std::string &spa) {
  if (spa == "MONO" || spa == "FC")
    return L"C";
  if (spa == "FL")
    return L"L";
  if (spa == "FR")
    return L"R";
  if (spa == "LFE")
    return L"LFE";
  if (spa == "RL")
    return L"RL";
  if (spa == "RR")
    return L"RR";
  if (spa == "SL")
    return L"SL";
  if (spa == "SR")
    return L"SR";
  throw std::runtime_error("unsupported channel position: " + spa);
}
