/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.cpp

Abstract:

    See header for contract. The timer is a single threadpool timer re-armed from its
    own callback: it fires once at SlowThreshold (to emit telemetry + log), then re-arms
    for the remainder of UserVisibleThreshold and fires a second time (to emit the
    user-visible warning). The destructor cancels and drains, so the callback is
    guaranteed not to touch *this after destruction.

--*/

#include "precomp.h"
#include "WslSlowOperation.h"

namespace
{
    FILETIME RelativeFileTime(std::chrono::milliseconds Relative) noexcept
    {
        // Negative FILETIME means "relative to now", in 100ns units.
        ULARGE_INTEGER due{};
        due.QuadPart = static_cast<ULONGLONG>(-(Relative.count() * 10'000LL));
        FILETIME ft{};
        ft.dwLowDateTime = due.LowPart;
        ft.dwHighDateTime = due.HighPart;
        return ft;
    }

    std::wstring WidenAscii(const char* Name) noexcept
    {
        // Phase names are ASCII identifiers (e.g. "WaitForMiniInitConnect"); widen byte-by-byte.
        std::wstring out;
        for (const char* p = Name; *p != '\0'; ++p)
        {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        }
        return out;
    }
}

WslSlowOperation::WslSlowOperation(
    _In_z_ const char* Name,
    std::chrono::milliseconds SlowThreshold,
    std::chrono::milliseconds UserVisibleThreshold) :
    m_name(Name),
    m_slowThreshold(SlowThreshold),
    m_userVisibleThreshold(UserVisibleThreshold),
    m_start(std::chrono::steady_clock::now()),
    m_uncaughtOnEntry(std::uncaught_exceptions())
{
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_timer);
    ArmTimer(m_slowThreshold);
}

WslSlowOperation::~WslSlowOperation() noexcept
{
    // Cancel any pending fire, then block until an in-flight callback drains. This is
    // the guarantee that OnTimerFired will not dereference `this` after destruction.
    if (m_timer)
    {
        SetThreadpoolTimer(m_timer.get(), nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_timer.get(), TRUE);
    }

    const Stage stage = m_stage.load(std::memory_order_acquire);
    if (stage == Stage::Fast)
    {
        return;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start);

    // Mirror the idiom used elsewhere in the codebase: avoid calling
    // wil::ResultFromCaughtException from a destructor (UB outside a catch handler);
    // use uncaught_exceptions() delta to surface "did this scope unwind?" only.
    const HRESULT hr = (std::uncaught_exceptions() > m_uncaughtOnEntry) ? E_FAIL : S_OK;

    try
    {
        WSL_LOG_TELEMETRY(
            "SlowOperationEnded",
            PDT_ProductAndServicePerformance,
            TraceLoggingString(m_name, "name"),
            TraceLoggingInt64(elapsed.count(), "elapsedMs"),
            TraceLoggingBool(stage == Stage::UserVisible, "userVisible"),
            TraceLoggingHResult(hr, "hr"));
    }
    CATCH_LOG()
}

void WslSlowOperation::ArmTimer(std::chrono::milliseconds Relative) noexcept
{
    FILETIME due = RelativeFileTime(Relative);
    SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
}

void CALLBACK WslSlowOperation::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
try
{
    auto* self = static_cast<WslSlowOperation*>(Context);

    Stage expected = Stage::Fast;
    if (self->m_stage.compare_exchange_strong(expected, Stage::Slow))
    {
        self->EmitSlowThresholdCrossed();
        const auto delta = self->m_userVisibleThreshold - self->m_slowThreshold;
        if (delta.count() > 0)
        {
            self->ArmTimer(delta);
        }
        return;
    }

    expected = Stage::Slow;
    if (self->m_stage.compare_exchange_strong(expected, Stage::UserVisible))
    {
        self->EmitUserVisibleThresholdCrossed();
    }
}
CATCH_LOG()

void WslSlowOperation::EmitSlowThresholdCrossed() noexcept
try
{
    WSL_LOG_TELEMETRY(
        "SlowOperationStarted",
        PDT_ProductAndServicePerformance,
        TraceLoggingString(m_name, "name"),
        TraceLoggingInt64(m_slowThreshold.count(), "thresholdMs"));

    WSL_LOG(
        "SlowOperation",
        TraceLoggingString(m_name, "name"),
        TraceLoggingInt64(m_slowThreshold.count(), "thresholdMs"));
}
CATCH_LOG()

void WslSlowOperation::EmitUserVisibleThresholdCrossed() noexcept
try
{
    // Non-localized English warning. The user-visible warning only fires on truly long
    // waits (UserVisibleThreshold, 60s by default) and the exact wording is not in the
    // localization pipeline yet; swap this for a Localization::MessageXxx call once the
    // resource is added. EMIT_USER_WARNING is a no-op if no ExecutionContext is on the
    // stack, so guards placed outside an ExecutionContext silently degrade to telemetry
    // + log only.
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(m_userVisibleThreshold).count();
    const auto message =
        std::format(L"WSL is still waiting on {} ({}s). This may take longer than usual.", WidenAscii(m_name), seconds);
    EMIT_USER_WARNING(message);
}
CATCH_LOG()
