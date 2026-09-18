// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <format>
#include <utility>
#include <wrl/client.h>
#include <wslc.h>

namespace wsl::windows::wslc::diagnostics {

inline bool IsEnabled(WSLCDiagnosticLevel enabledLevels, WSLCDiagnosticLevel level) noexcept
{
    return (static_cast<unsigned int>(enabledLevels) & static_cast<unsigned int>(level)) != 0;
}

inline WSLCDiagnosticLevel GetEnabledLevels(IDiagnosticCallback* callback) noexcept
{
    if (callback == nullptr)
    {
        return WSLCDiagnosticLevelNone;
    }

    WSLCDiagnosticLevel enabledLevels = WSLCDiagnosticLevelNone;
    const auto result = callback->GetEnabledLevels(&enabledLevels);
    if (FAILED(result))
    {
        LOG_HR(result);
        return WSLCDiagnosticLevelNone;
    }

    constexpr auto validLevels = static_cast<unsigned int>(WSLCDiagnosticLevelValid);
    if ((static_cast<unsigned int>(enabledLevels) & ~validLevels) != 0)
    {
        LOG_HR(E_INVALIDARG);
        return WSLCDiagnosticLevelNone;
    }

    return enabledLevels;
}

inline void Report(IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, const WSLCDiagnosticEvent& event) noexcept
{
    if (callback != nullptr && IsEnabled(enabledLevels, event.Level))
    {
        LOG_IF_FAILED(callback->OnDiagnostic(&event));
    }
}

inline void Report(IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) noexcept
{
    WSLCDiagnosticEvent event{};
    event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
    event.Level = level;
    event.Code = code;
    event.Message = message;
    Report(callback, enabledLevels, event);
}

template <typename... Args>
inline void Report(
    IDiagnosticCallback* callback,
    WSLCDiagnosticLevel enabledLevels,
    WSLCDiagnosticLevel level,
    LPCSTR code,
    std::wformat_string<Args...> format,
    Args&&... args) noexcept
{
    LOG_IF_FAILED(wil::ResultFromException([&]() {
        const auto message = std::format(std::move(format), std::forward<Args>(args)...);
        Report(callback, enabledLevels, level, code, message.c_str());
    }));
}

class DiagnosticReporter
{
public:
    explicit DiagnosticReporter(IDiagnosticCallback* callback) noexcept :
        m_callback(callback), m_enabledLevels(GetEnabledLevels(callback))
    {
    }

    bool HasCallback() const noexcept
    {
        return m_callback != nullptr;
    }

    bool IsEnabled(WSLCDiagnosticLevel level) const noexcept
    {
        return HasCallback() && wsl::windows::wslc::diagnostics::IsEnabled(m_enabledLevels, level);
    }

    WSLCDiagnosticLevel EnabledLevels() const noexcept
    {
        return m_enabledLevels;
    }

    void Report(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        wsl::windows::wslc::diagnostics::Report(m_callback.Get(), m_enabledLevels, level, code, message);
    }

    template <typename... Args>
    void Report(WSLCDiagnosticLevel level, LPCSTR code, std::wformat_string<Args...> format, Args&&... args) const noexcept
    {
        wsl::windows::wslc::diagnostics::Report(m_callback.Get(), m_enabledLevels, level, code, std::move(format), std::forward<Args>(args)...);
    }

private:
    Microsoft::WRL::ComPtr<IDiagnosticCallback> m_callback;
    WSLCDiagnosticLevel m_enabledLevels = WSLCDiagnosticLevelNone;
};

} // namespace wsl::windows::wslc::diagnostics

// Diagnostic arguments are evaluated only when the requested level is enabled.
#define WSLC_DIAG(Context, Level, ...) \
    do \
    { \
        auto&& _wslcDiagnosticContext = (Context); \
        const auto _wslcDiagnosticLevel = (Level); \
        if (_wslcDiagnosticContext.IsEnabled(_wslcDiagnosticLevel)) \
        { \
            _wslcDiagnosticContext.Report(_wslcDiagnosticLevel, __VA_ARGS__); \
        } \
    } while (false)
