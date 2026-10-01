/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxsstest.h

Abstract:

    Common definitions for lxss tests.

--*/

#pragma once

#define STR2WSTR_INNER(str) L##str
#define STR2WSTR(str) STR2WSTR_INNER(str)

//
// Logging macros
//

#define LogError(str, ...) \
    { \
        WEX::Logging::Log::Error(WEX::Common::String().Format(STR2WSTR(str), __VA_ARGS__)); \
    }

#define LogInfo(str, ...) \
    { \
        WEX::Logging::Log::Comment(WEX::Common::String().Format(STR2WSTR(str), __VA_ARGS__)); \
    }

#define LogWarning(str, ...) \
    { \
        WEX::Logging::Log::Warning(WEX::Common::String().Format(STR2WSTR(str), __VA_ARGS__)); \
    }

#define LogPass(str, ...) \
    { \
        WEX::Logging::Log::Result(WEX::Logging::TestResults::Passed, WEX::Common::String().Format(STR2WSTR(str), __VA_ARGS__)); \
    }

#define LogSkipped(str, ...) \
    { \
        WEX::Logging::Log::Result(WEX::Logging::TestResults::Skipped, WEX::Common::String().Format(STR2WSTR(str), __VA_ARGS__)); \
    }

//
// Helper macros
//

#define ALLOC(_size) HeapAlloc(GetProcessHeap(), 0, (_size))
#define FREE(_ptr) WI_VERIFY(HeapFree(GetProcessHeap(), 0, (_ptr)) != FALSE)
