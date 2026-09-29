#include "stdafx.h"
#include "helpers/LogHelper.h"
#include "helpers/StringHelper.h"
#include <cstdarg>
#include <iostream>
#include <locale>
#include <codecvt>

bool LogHelper::initialized = false;
std::wstring LogHelper::logPath;
bool LogHelper::enableTrace = false;
FILE* LogHelper::presetFP = nullptr;
bool LogHelper::compact = true;
bool LogHelper::useConsoleColors = false;

void LogHelper::log(const char*, int, const void*, bool trace, const wchar_t* format, ...)
{
    if (trace) return;
    std::wstring linuxFormat;
    for (size_t i=0; format[i]; ++i) {
        linuxFormat += format[i];
        if (format[i] != L'%') continue;
        if (format[i+1] == L'%') { linuxFormat += format[++i]; continue; }
        size_t j=i+1;
        while (format[j] && !std::iswalpha(format[j])) ++j;
        if (!format[j]) break;
        for(size_t k=i+1;k<j;++k) linuxFormat += format[k];
        if (format[j] == L's') {
            if (j==i+1 || format[j-1]!=L'l') linuxFormat += L'l';
            linuxFormat += L's';
        } else if (format[j] == L'S') linuxFormat += L's';
        else linuxFormat += format[j];
        i=j;
    }
    wchar_t text[2048];
    va_list args;
    va_start(args, format);
    std::vswprintf(text, sizeof(text) / sizeof(*text), linuxFormat.c_str(), args);
    va_end(args);
    std::wcerr << L"skyapo: " << text << L'\n';
}
void LogHelper::reset() {}
void LogHelper::set(FILE*, bool, bool, bool) {}

std::wstring StringHelper::replaceCharacters(const std::wstring& s, const std::wstring& chars, const std::wstring& replacement)
{
    std::wstring out;
    for (wchar_t c : s) out += chars.find(c) == std::wstring::npos ? std::wstring(1, c) : replacement;
    return out;
}
std::wstring StringHelper::replaceIllegalCharacters(const std::wstring& s) { return s; }
std::wstring StringHelper::toWString(const std::string& s, unsigned) { return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(s); }
std::string StringHelper::toString(const std::wstring& s, unsigned) { return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(s); }
std::wstring StringHelper::toLowerCase(const std::wstring& s) { auto r=s; std::transform(r.begin(),r.end(),r.begin(),::towlower); return r; }
std::wstring StringHelper::toUpperCase(const std::wstring& s) { auto r=s; std::transform(r.begin(),r.end(),r.begin(),::towupper); return r; }
std::wstring StringHelper::trim(const std::wstring& s) { const auto a=s.find_first_not_of(L" \t\r\n"), b=s.find_last_not_of(L" \t\r\n"); return a==s.npos?L"":s.substr(a,b-a+1); }
std::vector<std::wstring> StringHelper::split(const std::wstring& s, wchar_t c, bool skip) { std::vector<std::wstring> v; size_t p=0; while(p<=s.size()){auto n=s.find(c,p); auto x=s.substr(p,n==s.npos?n:n-p); if(!skip||!x.empty())v.push_back(x); if(n==s.npos)break;p=n+1;} return v; }
std::wstring StringHelper::join(const std::vector<std::wstring>& v, const std::wstring& sep) { std::wstring s; for(size_t i=0;i<v.size();++i){if(i)s+=sep;s+=v[i];}return s; }
std::wstring StringHelper::getSystemErrorString(long) { return L"system error"; }
std::vector<std::wstring> StringHelper::splitQuoted(const std::wstring& s, wchar_t c, wchar_t) { return split(s,c); }
