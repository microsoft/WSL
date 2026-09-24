// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "stringshared.h"
#include <format>
#include <functional>
#include <utility>

namespace wsl::windows::wslc::events {

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
