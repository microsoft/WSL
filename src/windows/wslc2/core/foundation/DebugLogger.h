// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Logging.h"

namespace wsl::windows::wslc::logging
{
    // Sends logs to the OutputDebugString function.
    // Intended for use during initialization debugging.
    struct OutputDebugLogger : ILogger
    {
        OutputDebugLogger() = default;

        ~OutputDebugLogger() = default;

        // ILogger
        std::wstring GetName() const override;

        void Write(Channel channel, Level, std::wstring_view message) noexcept override;

        void WriteDirect(Channel channel, Level level, std::wstring_view message) noexcept override;

        // Adds OutputDebugLogger to the current Log
        static void Add();

        // Removes OutputDebugLogger from the current Log
        static void Remove();
    };
}
