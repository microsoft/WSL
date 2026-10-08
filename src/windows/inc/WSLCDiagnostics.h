// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WSLCEvent.h"
#include "WslTelemetry.h"
#include <functional>
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

using CallbackScope = std::function<HRESULT(const std::function<HRESULT()>&)>;

template <typename Callback>
inline HRESULT InvokeCallback(const CallbackScope& callbackScope, Callback&& callback) noexcept
{
    if (!callbackScope)
    {
        return callback();
    }

    try
    {
        return callbackScope(std::function<HRESULT()>(std::forward<Callback>(callback)));
    }
    CATCH_RETURN();
}

inline WSLCDiagnosticLevel GetEnabledLevels(IDiagnosticCallback* callback, HRESULT* queryResult = nullptr, const CallbackScope& callbackScope = {}) noexcept
{
    if (callback == nullptr)
    {
        if (queryResult != nullptr)
        {
            *queryResult = S_FALSE;
        }

        return WSLCDiagnosticLevelNone;
    }

    WSLCDiagnosticLevel enabledLevels = WSLCDiagnosticLevelNone;
    const auto result = InvokeCallback(callbackScope, [&]() { return callback->GetEnabledLevels(&enabledLevels); });
    if (FAILED(result))
    {
        LOG_HR(result);
        if (queryResult != nullptr)
        {
            *queryResult = result;
        }

        return WSLCDiagnosticLevelNone;
    }

    constexpr auto validLevels = static_cast<unsigned int>(WSLCDiagnosticLevelValid);
    if ((static_cast<unsigned int>(enabledLevels) & ~validLevels) != 0)
    {
        LOG_HR(E_INVALIDARG);
        if (queryResult != nullptr)
        {
            *queryResult = E_INVALIDARG;
        }

        return WSLCDiagnosticLevelNone;
    }

    if (queryResult != nullptr)
    {
        *queryResult = S_OK;
    }

    return enabledLevels;
}

inline HRESULT TryReport(IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, const WSLCDiagnosticEvent& event) noexcept
{
    if (callback == nullptr || !IsEnabled(enabledLevels, event.Level))
    {
        return S_FALSE;
    }

    return callback->OnDiagnostic(&event);
}

inline HRESULT TryReport(
    IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) noexcept
{
    WSLCDiagnosticEvent event{};
    event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
    event.Timestamp = timestamp;
    event.Level = level;
    event.Code = code;
    event.Message = message;
    return TryReport(callback, enabledLevels, event);
}

inline void Report(IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) noexcept
{
    LOG_IF_FAILED(TryReport(callback, enabledLevels, timestamp, level, code, message));
}

inline void Report(IDiagnosticCallback* callback, WSLCDiagnosticLevel enabledLevels, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) noexcept
{
    Report(callback, enabledLevels, events::GetCurrentTimestamp(), level, code, message);
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
    if (callback == nullptr || !IsEnabled(enabledLevels, level))
    {
        return;
    }

    LOG_IF_FAILED(wil::ResultFromException([&]() {
        const auto message = std::format(std::move(format), std::forward<Args>(args)...);
        Report(callback, enabledLevels, level, code, message.c_str());
    }));
}

class DiagnosticReporter
{
public:
    explicit DiagnosticReporter(IDiagnosticCallback* callback, CallbackScope callbackScope = {}) noexcept :
        m_callback(callback), m_callbackScope(std::move(callbackScope))
    {
        m_enabledLevels = GetEnabledLevels(callback, &m_enabledLevelsResult, m_callbackScope);
    }

    bool HasCallback() const noexcept
    {
        return m_callback != nullptr;
    }

    bool IsCallbackReady() const noexcept
    {
        return HasCallback() && SUCCEEDED(m_enabledLevelsResult);
    }

    bool IsEnabled(WSLCDiagnosticLevel level) const noexcept
    {
        return IsCallbackReady() && wsl::windows::wslc::diagnostics::IsEnabled(m_enabledLevels, level);
    }

    WSLCDiagnosticLevel EnabledLevels() const noexcept
    {
        return m_enabledLevels;
    }

    HRESULT TryReport(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        if (!IsEnabled(level))
        {
            return S_FALSE;
        }

        WSLCDiagnosticEvent event{};
        event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
        event.Timestamp = events::GetCurrentTimestamp();
        event.Level = level;
        event.Code = code;
        event.Message = message;
        return InvokeCallback(m_callbackScope, [&]() { return m_callback->OnDiagnostic(&event); });
    }

    void Report(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        LOG_IF_FAILED(TryReport(level, code, message));
    }

    void ReportAt(ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        if (!IsEnabled(level))
        {
            return;
        }

        WSLCDiagnosticEvent event{};
        event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
        event.Timestamp = timestamp;
        event.Level = level;
        event.Code = code;
        event.Message = message;
        LOG_IF_FAILED(InvokeCallback(m_callbackScope, [&]() { return m_callback->OnDiagnostic(&event); }));
    }

    template <typename... Args>
    void Report(WSLCDiagnosticLevel level, LPCSTR code, std::wformat_string<Args...> format, Args&&... args) const noexcept
    {
        if (!IsEnabled(level))
        {
            return;
        }

        LOG_IF_FAILED(wil::ResultFromException([&]() {
            const auto message = std::format(std::move(format), std::forward<Args>(args)...);
            Report(level, code, message.c_str());
        }));
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
            Report(level, code, wideMessage.c_str());
        }));
    }

private:
    Microsoft::WRL::ComPtr<IDiagnosticCallback> m_callback;
    CallbackScope m_callbackScope;
    WSLCDiagnosticLevel m_enabledLevels = WSLCDiagnosticLevelNone;
    HRESULT m_enabledLevelsResult = S_FALSE;
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
        const bool _wslcEventTraceEnabled = ::wsl::windows::wslc::events::IsTraceEnabled(); \
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
