// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WSLCEvent.h"
#include "WslTelemetry.h"
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>
#include <wrl/implements.h>
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

inline bool IsEventLevel(WSLCDiagnosticLevel level) noexcept
{
    switch (level)
    {
    case WSLCDiagnosticLevelDebug:
    case WSLCDiagnosticLevelWarning:
        return true;
    case WSLCDiagnosticLevelNone:
        return false;
    default:
        return false;
    }
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
        return TryReportAt(events::GetCurrentTimestamp(), level, code, message);
    }

    void Report(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        LOG_IF_FAILED(TryReport(level, code, message));
    }

    void ReportAt(ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr) const noexcept
    {
        LOG_IF_FAILED(TryReportAt(timestamp, level, code, message));
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
        if (!IsEnabled(level))
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
    HRESULT TryReportAt(ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message) const noexcept
    {
        RETURN_HR_IF(E_INVALIDARG, !IsEventLevel(level));
        RETURN_HR_IF(E_INVALIDARG, code == nullptr || code[0] == '\0');
        if (!IsEnabled(level))
        {
            return S_FALSE;
        }

        WSLCDiagnosticEvent event{};
        event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
        event.Timestamp = timestamp;
        event.Level = level;
        event.Code = code;
        event.Message = message;
        return InvokeCallback(m_callbackScope, [&]() { return m_callback->OnDiagnostic(&event); });
    }

    Microsoft::WRL::ComPtr<IDiagnosticCallback> m_callback;
    CallbackScope m_callbackScope;
    WSLCDiagnosticLevel m_enabledLevels = WSLCDiagnosticLevelNone;
    HRESULT m_enabledLevelsResult = S_FALSE;
};

// Copies share event data and enabled levels, but never retain a client callback.
class DiagnosticQueue
{
public:
    explicit DiagnosticQueue(WSLCDiagnosticLevel enabledLevels = WSLCDiagnosticLevelNone)
    {
        if (enabledLevels != WSLCDiagnosticLevelNone)
        {
            m_state = std::make_shared<State>(enabledLevels);
        }
    }

    WSLCDiagnosticLevel EnabledLevels() const noexcept
    {
        if (!m_state)
        {
            return WSLCDiagnosticLevelNone;
        }

        std::lock_guard lock(m_state->Lock);
        return m_state->EnabledLevels;
    }

    bool IsEnabled(WSLCDiagnosticLevel level) const noexcept
    {
        return diagnostics::IsEnabled(EnabledLevels(), level);
    }

    void Report(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message)
    {
        ReportAt(events::GetCurrentTimestamp(), level, code, message);
    }

    void ReportAt(ULONGLONG timestamp, WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message)
    {
        THROW_HR_IF(E_INVALIDARG, !IsEventLevel(level));
        THROW_HR_IF(E_INVALIDARG, code == nullptr || code[0] == '\0');
        if (!m_state)
        {
            return;
        }

        std::lock_guard lock(m_state->Lock);
        if (diagnostics::IsEnabled(m_state->EnabledLevels, level))
        {
            m_state->Pending.push_back({timestamp, level, code, message != nullptr ? std::optional<std::wstring>{message} : std::nullopt});
        }
    }

    // The originating operation closes the queue and delivers its events before returning.
    void Complete(const DiagnosticReporter& reporter) noexcept
    {
        if (!m_state)
        {
            return;
        }

        std::vector<PendingEvent> pending;
        {
            std::lock_guard lock(m_state->Lock);
            m_state->EnabledLevels = WSLCDiagnosticLevelNone;
            pending.swap(m_state->Pending);
        }

        for (const auto& event : pending)
        {
            reporter.ReportAt(event.Timestamp, event.Level, event.Code.c_str(), event.Message ? event.Message->c_str() : nullptr);
        }
    }

private:
    struct PendingEvent
    {
        ULONGLONG Timestamp;
        WSLCDiagnosticLevel Level;
        std::string Code;
        std::optional<std::wstring> Message;
    };

    struct State
    {
        explicit State(WSLCDiagnosticLevel enabledLevels) : EnabledLevels(enabledLevels)
        {
        }

        std::mutex Lock;
        _Guarded_by_(Lock) WSLCDiagnosticLevel EnabledLevels;
        _Guarded_by_(Lock) std::vector<PendingEvent> Pending;
    };

    std::shared_ptr<State> m_state;
};

class QueuedDiagnosticCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDiagnosticCallback, IFastRundown>
{
public:
    explicit QueuedDiagnosticCallback(const DiagnosticQueue& queue) : m_queue(queue), m_enabledLevels(queue.EnabledLevels())
    {
    }

    HRESULT GetEnabledLevels(WSLCDiagnosticLevel* levels) override
    {
        RETURN_HR_IF_NULL(E_POINTER, levels);
        *levels = m_enabledLevels;
        return S_OK;
    }

    HRESULT OnDiagnostic(const WSLCDiagnosticEvent* event) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, event);
        RETURN_HR_IF(E_INVALIDARG, event->SchemaVersion != WSLC_DIAGNOSTIC_SCHEMA_VERSION);
        m_queue.ReportAt(event->Timestamp, event->Level, event->Code, event->Message);
        return S_OK;
    }
    CATCH_RETURN()

private:
    DiagnosticQueue m_queue;
    const WSLCDiagnosticLevel m_enabledLevels;
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
