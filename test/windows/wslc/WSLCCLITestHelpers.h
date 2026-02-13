/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTestHelpers.h

Abstract:

    Helper utilities for WSLC CLI unit tests.

--*/

#pragma once

#include <string>
#include <Windows.h>
#include <WexTestClass.h>

namespace WSLCTestHelpers {
// Helper function to convert wstring to UTF-8 string for TAEF logging
inline std::string WStringToUTF8(const std::wstring& wstr)
{
    if (wstr.empty())
    {
        return std::string();
    }

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], size_needed, nullptr, nullptr);
    return result;
}

// Convenience wrapper for Log::Comment with wstring
inline void LogComment(const std::wstring& message)
{
    WEX::Logging::Log::Comment(reinterpret_cast<const char8_t*>(WStringToUTF8(message).c_str()));
}
} // namespace WSLCTestHelpers