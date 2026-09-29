#pragma once
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cwchar>
#include <new>
#include <cwctype>
#include <sndfile.h>
#define swscanf_s swscanf
#ifndef _WIN32
#define __forceinline inline
#define __declspec(x) __attribute__((x))
#endif
#define AVRT_VTABLES_BEGIN
#define AVRT_VTABLES_END
#define AVRT_CODE_BEGIN
#define AVRT_CODE_END

SNDFILE *sf_wchar_open(const wchar_t *path, int mode, SF_INFO *info);
