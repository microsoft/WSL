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
#include "Invocation.h"

namespace WSLCTestHelpers {

inline wsl::windows::wslc::Invocation CreateInvocationFromCommandLine(const std::wstring& commandLine)
{
    // Simulate creation of Arvc/Argc from command line as Windows does.
    int argc = 0;
    wil::unique_hlocal_ptr<LPWSTR[]> argv;
    argv.reset(CommandLineToArgvW(commandLine.c_str(), &argc));
    VERIFY_IS_NOT_NULL(argv.get());
    VERIFY_IS_GREATER_THAN(argc, 0);

    // Convert to vector for Invocation, skipping argv[0] (executable path)
    // This is what we do in wmain() to populate Invocation input vector.
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)  // Skip argv[0]
    {
        args.push_back(argv[i]);
    }

    return wsl::windows::wslc::Invocation(std::move(args), commandLine.c_str());
}

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