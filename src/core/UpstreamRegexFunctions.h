/*
    This file adapts the Equalizer APO parser regex functions for SkyAPO.
    Copyright (C) 2014  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    This implementation follows upstream parser/RegexFunctions.cpp. The
    original Windows-only integration is omitted; the wide std::regex behavior
    used by the configuration parser is retained.
*/

#pragma once

#include <mpParser.h>

#include <regex>
#include <string>
#include <vector>

namespace skyapo::config {

class RegexSearchFunction final : public mup::ICallback {
public:
  RegexSearchFunction() : ICallback(mup::cmFUNC, L"regexSearch", 2) {}

  void Eval(mup::ptr_val_type &result, const mup::ptr_val_type *arguments,
            int) override {
    requireString(arguments[0], 1);
    requireString(arguments[1], 2);
    const std::wstring pattern = arguments[0]->GetString();
    const std::wstring input = arguments[1]->GetString();
    const std::wregex expression(pattern);
    std::wsmatch match;
    std::vector<mup::Value> values;
    if (std::regex_search(input, match, expression)) {
      values.reserve(match.size());
      for (size_t i = 0; i < match.size(); ++i)
        values.emplace_back(match.str(i));
    }
    *result = values;
  }

  const mup::char_type *GetDesc() const override {
    return L"regexSearch - searches first match to regular expression; result "
           L"is empty if not matching";
  }

  mup::IToken *Clone() const override { return new RegexSearchFunction(*this); }

private:
  void requireString(const mup::ptr_val_type &value, int argument) const {
    if (!value->IsString())
      throw mup::ParserError(mup::ErrorContext(
          mup::ecTYPE_CONFLICT_FUN, -1, GetIdent(), value->GetType(), 's',
          argument));
  }
};

class RegexReplaceFunction final : public mup::ICallback {
public:
  RegexReplaceFunction() : ICallback(mup::cmFUNC, L"regexReplace", 3) {}

  void Eval(mup::ptr_val_type &result, const mup::ptr_val_type *arguments,
            int) override {
    requireString(arguments[0], 1);
    requireString(arguments[1], 2);
    requireString(arguments[2], 3);
    const std::wregex expression(arguments[0]->GetString());
    *result = std::regex_replace(arguments[1]->GetString(), expression,
                                 arguments[2]->GetString());
  }

  const mup::char_type *GetDesc() const override {
    return L"regexReplace - replaces all regular-expression matches";
  }

  mup::IToken *Clone() const override {
    return new RegexReplaceFunction(*this);
  }

private:
  void requireString(const mup::ptr_val_type &value, int argument) const {
    if (!value->IsString())
      throw mup::ParserError(mup::ErrorContext(
          mup::ecTYPE_CONFLICT_FUN, -1, GetIdent(), value->GetType(), 's',
          argument));
  }
};

} // namespace skyapo::config
