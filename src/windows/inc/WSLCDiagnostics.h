// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WSLCEvent.h"
#include "WslTelemetry.h"
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <wrl/client.h>
#include <wslc.h>

namespace wsl::windows::wslc::diagnostics {

struct SanitizationRule
{
    std::wstring_view Value;
    std::wstring_view Replacement;
};

inline std::wstring SanitizeString(std::wstring_view value, std::initializer_list<SanitizationRule> rules)
{
    std::wstring sanitized{value};
    for (const auto& rule : rules)
    {
        if (rule.Value.empty())
        {
            continue;
        }

        size_t searchOffset = 0;
        while (searchOffset < sanitized.size())
        {
            THROW_HR_IF(
                E_INVALIDARG,
                sanitized.size() - searchOffset > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    rule.Value.size() > static_cast<size_t>(std::numeric_limits<int>::max()));

            const auto match = FindStringOrdinal(
                FIND_FROMSTART,
                sanitized.data() + searchOffset,
                static_cast<int>(sanitized.size() - searchOffset),
                rule.Value.data(),
                static_cast<int>(rule.Value.size()),
                TRUE);
            if (match < 0)
            {
                break;
            }

            const auto matchOffset = searchOffset + static_cast<size_t>(match);
            sanitized.replace(matchOffset, rule.Value.size(), rule.Replacement);
            searchOffset = matchOffset + rule.Replacement.size();
        }
    }

    return sanitized;
}

inline std::wstring SanitizeUserName(std::wstring_view value, std::wstring_view userName)
{
    return SanitizeString(value, {{userName, L"<user>"}});
}

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

    template <typename... Args>
    void Report(WSLCDiagnosticLevel level, LPCSTR code, std::format_string<Args...> format, Args&&... args) const noexcept
    {
        if (!HasCallback() || !IsEnabled(level))
        {
            return;
        }

        LOG_IF_FAILED(wil::ResultFromException([&]() {
            const auto message = std::format(std::move(format), std::forward<Args>(args)...);
            const auto wideMessage = wsl::shared::string::MultiByteToWide(message);
            wsl::windows::wslc::diagnostics::Report(m_callback.Get(), m_enabledLevels, level, code, wideMessage.c_str());
        }));
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
            LOG_IF_FAILED(wil::ResultFromException([&]() { _wslcDiagnosticContext.Report(_wslcDiagnosticLevel, __VA_ARGS__); })); \
        } \
    } while (false)

// Use WSLC_EVENT for command-scoped events that help explain command behavior,
// operational decisions, progress, or failures and are suitable for caller-visible
// debug output. Messages must be relevant to the current operation, bounded, and safe
// to redirect or share. Do not include credentials, authentication data, request or
// response bodies, environment values, file contents, unsanitized URLs, or data
// unrelated to the caller. Use WSL_LOG for trace-only events and component-level
// implementation details.
//
// Events are always available through TraceLogging and are mirrored to the operation's
// diagnostic callback when Debug is enabled. Message arguments are evaluated once and
// only when either sink is enabled.
#define WSLC_EVENT(Context, Name, Code, Format, ...) \
    do \
    { \
        auto&& _wslcEventContext = (Context); \
        const bool _wslcEventTraceEnabled = \
            g_hTraceLoggingProvider != nullptr && TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_VERBOSE, 0); \
        const bool _wslcEventDiagnosticEnabled = _wslcEventContext.IsEnabled(WSLCDiagnosticLevelDebug); \
        ::wsl::windows::wslc::events::Dispatch( \
            _wslcEventTraceEnabled, \
            _wslcEventDiagnosticEnabled, \
            [&]() { return ::wsl::windows::wslc::events::FormatMessage((Format), __VA_ARGS__); }, \
            [&](const std::wstring& _wslcEventMessage) { \
                WSL_LOG( \
                    Name, \
                    TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE), \
                    TraceLoggingString((Code), "DiagnosticCode"), \
                    TraceLoggingWideString(_wslcEventMessage.c_str(), "Message")); \
            }, \
            [&](const std::wstring& _wslcEventMessage) { \
                _wslcEventContext.Report(WSLCDiagnosticLevelDebug, (Code), _wslcEventMessage.c_str()); \
            }); \
    } while (false)
