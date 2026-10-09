// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "stringshared.h"
#include "WslTelemetry.h"
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace wsl::windows::wslc::events {

inline bool IsTraceEnabled() noexcept
{
    return g_hTraceLoggingProvider != nullptr && TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_VERBOSE, 0);
}

inline ULONGLONG GetCurrentTimestamp() noexcept
{
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);

    ULARGE_INTEGER timestamp{};
    timestamp.LowPart = fileTime.dwLowDateTime;
    timestamp.HighPart = fileTime.dwHighDateTime;
    return timestamp.QuadPart;
}

inline std::wstring EscapeControlCharacters(std::wstring_view message)
{
    constexpr wchar_t c_hexDigits[] = L"0123456789ABCDEF";

    std::wstring escaped;
    escaped.reserve(message.size());
    for (const auto character : message)
    {
        switch (character)
        {
        case L'\r':
            escaped.append(L"\\r");
            break;

        case L'\n':
            escaped.append(L"\\n");
            break;

        case L'\t':
            escaped.append(L"\\t");
            break;

        default:
            if (character < L' ' || character == L'\x7f')
            {
                const auto value = static_cast<unsigned int>(character);
                escaped.append(L"\\u00");
                escaped.push_back(c_hexDigits[(value >> 4) & 0xf]);
                escaped.push_back(c_hexDigits[value & 0xf]);
            }
            else
            {
                escaped.push_back(character);
            }
            break;
        }
    }

    return escaped;
}

inline std::wstring FormatDebugEvent(LPCSTR code, std::wstring_view message, ULONGLONG timestamp = 0)
{
    if (timestamp == 0)
    {
        timestamp = GetCurrentTimestamp();
    }

    ULARGE_INTEGER timestampValue{};
    timestampValue.QuadPart = timestamp;

    FILETIME fileTime{};
    fileTime.dwLowDateTime = timestampValue.LowPart;
    fileTime.dwHighDateTime = timestampValue.HighPart;

    SYSTEMTIME utcTime{};
    THROW_LAST_ERROR_IF(!FileTimeToSystemTime(&fileTime, &utcTime));

    const auto escapedMessage = EscapeControlCharacters(message);
    const auto prefix = std::format(
        L"[debug] {:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z [{}]",
        utcTime.wYear,
        utcTime.wMonth,
        utcTime.wDay,
        utcTime.wHour,
        utcTime.wMinute,
        utcTime.wSecond,
        utcTime.wMilliseconds,
        wsl::shared::string::MultiByteToWide(code));
    return escapedMessage.empty() ? std::format(L"{}\n", prefix) : std::format(L"{} {}\n", prefix, escapedMessage);
}

template <typename... Args>
std::wstring FormatMessage(std::wformat_string<Args...> format, Args&&... args)
{
    return std::format(std::move(format), std::forward<Args>(args)...);
}

template <typename... Args>
std::wstring FormatMessage(std::format_string<Args...> format, Args&&... args)
{
    return wsl::shared::string::MultiByteToWide(std::format(std::move(format), std::forward<Args>(args)...));
}

template <typename MessageFactory, typename TraceWriter, typename OutputWriter>
void Dispatch(bool traceEnabled, bool outputEnabled, MessageFactory&& messageFactory, TraceWriter&& traceWriter, OutputWriter&& outputWriter) noexcept
{
    if (!traceEnabled && !outputEnabled)
    {
        return;
    }

    LOG_IF_FAILED(wil::ResultFromException([&]() {
        const auto message = std::invoke(std::forward<MessageFactory>(messageFactory));
        if (traceEnabled)
        {
            std::invoke(std::forward<TraceWriter>(traceWriter), message);
        }

        if (outputEnabled)
        {
            std::invoke(std::forward<OutputWriter>(outputWriter), message);
        }
    }));
}

} // namespace wsl::windows::wslc::events
