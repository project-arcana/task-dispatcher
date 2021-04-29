#pragma once

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#ifdef TD_BUILD_DLL

#ifdef TD_DLL
#define TD_API __declspec(dllexport)
#else
#define TD_API __declspec(dllimport)
#endif

#else
#define TD_API
#endif

#else
#define TD_API
#endif
