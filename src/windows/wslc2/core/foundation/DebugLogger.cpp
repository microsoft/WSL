// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "DebugLogger.h"

namespace wsl::windows::wslc::logging
{
    namespace
    {
        static constexpr std::wstring_view s_OutputDebugLoggerName = L"OutputDebugLogger";
    }

    std::wstring OutputDebugLogger::GetName() const
    {
        return std::wstring{ s_OutputDebugLoggerName };
    }

    void OutputDebugLogger::Write(Channel channel, Level, std::wstring_view message) noexcept try
    {
        std::wostringstream strstr;
        strstr << L"[" << std::setw(GetMaxChannelNameLength()) << std::left << std::setfill(L' ') << GetChannelName(channel) << L"] " << message << std::endl;
        std::wstring formattedMessage = std::move(strstr).str();

        OutputDebugStringW(formattedMessage.c_str());
    }
    catch (...)
    {
        // Just eat any exceptions here; better than losing logs
    }

    void OutputDebugLogger::WriteDirect(Channel, Level, std::wstring_view message) noexcept try
    {
        std::wstring nullTerminatedMessage{ message };
        OutputDebugStringW(nullTerminatedMessage.c_str());
    }
    catch (...)
    {
        // Just eat any exceptions here; better than losing logs
    }

    void OutputDebugLogger::Add()
    {
        Log().AddLogger(std::make_unique<OutputDebugLogger>());
    }

    void OutputDebugLogger::Remove()
    {
        Log().RemoveLogger(std::wstring{ s_OutputDebugLoggerName });
    }
}
