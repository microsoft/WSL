/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleState.cpp

Abstract:

    This file contains function definitions for the ConsoleState helper class.

--*/

#include "precomp.h"
#include "ConsoleState.h"
#pragma hdrstop

namespace {

void ChangeConsoleMode(_In_ HANDLE Handle, _In_ DWORD Mode)
{
    // Use the invalid parameter error code to detect the v1 console that does not support the provided mode.
    // This can be improved in the future when a more elegant solution exists.
    //
    // N.B. Ignore failures setting the mode if the console has already disconnected.
    if (!SetConsoleMode(Handle, Mode))
    {
        // DISABLE_NEWLINE_AUTO_RETURN is not supported everywhere, if the flag was present fall back and try again.
        if (WI_IsFlagSet(Mode, DISABLE_NEWLINE_AUTO_RETURN))
        {
            Mode = WI_ClearFlag(Mode, DISABLE_NEWLINE_AUTO_RETURN);
            if (SetConsoleMode(Handle, Mode))
            {
                return;
            }
        }

        switch (GetLastError())
        {
        case ERROR_PIPE_NOT_CONNECTED:
            break;

        case ERROR_INVALID_PARAMETER:
            THROW_HR_MSG(WSL_E_CONSOLE, "SetConsoleMode(0x%x) failed", Mode);

        default:
            THROW_LAST_ERROR_MSG("SetConsoleMode(0x%x) failed", Mode);
        }
    }
}

void TrySetConsoleMode(_In_ HANDLE Handle, _In_ DWORD Mode)
try
{
    ChangeConsoleMode(Handle, Mode);
}
CATCH_LOG()

std::optional<DWORD> TryGetConsoleMode(_In_ HANDLE Handle)
{
    DWORD mode{};
    if (!GetConsoleMode(Handle, &mode))
    {
        const auto error = GetLastError();
        if ((error != ERROR_PIPE_NOT_CONNECTED) && (error != ERROR_INVALID_HANDLE))
        {
            LOG_WIN32_MSG(error, "GetConsoleMode failed");
        }

        return std::nullopt;
    }

    return mode;
}

struct ConsoleValues
{
    DWORD InputMode;
    UINT InputCodePage;
    DWORD OutputMode;
    UINT OutputCodePage;
};

struct CoordinationState
{
    struct Owner
    {
        DWORD ProcessId;
        ULONG References;
    };

    ULONG Version;
    ConsoleValues Baseline;
    ConsoleValues Configured;
    Owner Owners[128];
};

class MutexLock
{
public:
    explicit MutexLock(HANDLE mutex) : m_mutex(mutex)
    {
        const auto result = WaitForSingleObject(mutex, INFINITE);
        THROW_HR_IF(E_UNEXPECTED, (result != WAIT_OBJECT_0) && (result != WAIT_ABANDONED));
    }

    ~MutexLock()
    {
        LOG_IF_WIN32_BOOL_FALSE(ReleaseMutex(m_mutex));
    }

private:
    HANDLE m_mutex;
};

ConsoleValues CaptureConsoleValues(HANDLE input, HANDLE output)
{
    ConsoleValues values{};
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleMode(input, &values.InputMode));
    values.InputCodePage = GetConsoleCP();
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleMode(output, &values.OutputMode));
    values.OutputCodePage = GetConsoleOutputCP();
    return values;
}

void ApplyConsoleValues(HANDLE input, HANDLE output, const ConsoleValues& values)
{
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(values.InputCodePage));
    TrySetConsoleMode(input, values.InputMode);
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(values.OutputCodePage));
    TrySetConsoleMode(output, values.OutputMode);
}

bool IsOwnerAlive(const CoordinationState::Owner& owner)
{
    wil::unique_handle process{OpenProcess(SYNCHRONIZE, FALSE, owner.ProcessId)};
    if (!process)
    {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }

    return WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0;
}

} // namespace

namespace wsl::windows::common {

ConsoleState::ConsoleState(RestorePolicy Policy) : m_restorePolicy(Policy)
{
    m_InputHandle.reset(
        CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    if (!m_InputHandle)
    {
        LOG_LAST_ERROR_MSG("CreateFileW(CONIN$) failed");
    }

    m_OutputHandle.reset(
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    if (!m_OutputHandle)
    {
        LOG_LAST_ERROR_MSG("CreateFileW(CONOUT$) failed");
    }
}

void ConsoleState::SetInteractiveMode()
{
    if (m_interactiveModeConfigured)
    {
        return;
    }

    if ((m_restorePolicy == RestorePolicy::OnlyIfUnchanged) && AcquireCoordination())
    {
        m_interactiveModeConfigured = true;
        return;
    }

    // Ensure console state is restored if this method throws.
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { RestoreConsoleState(); });

    if (m_InputHandle)
    {
        m_SavedInputCodePage = GetConsoleCP();
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));
        if (m_restorePolicy == RestorePolicy::OnlyIfUnchanged)
        {
            m_ConfiguredInputCodePage = GetConsoleCP();
        }

        // Configure for raw input with VT support.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_InputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        WI_ClearAllFlags(newMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        ChangeConsoleMode(m_InputHandle.get(), newMode);
        m_SavedInputMode = mode;
        if (m_restorePolicy == RestorePolicy::OnlyIfUnchanged)
        {
            m_ConfiguredInputMode = TryGetConsoleMode(m_InputHandle.get()).value_or(newMode);
        }
    }

    if (m_OutputHandle)
    {
        m_SavedOutputCodePage = GetConsoleOutputCP();
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
        if (m_restorePolicy == RestorePolicy::OnlyIfUnchanged)
        {
            m_ConfiguredOutputCodePage = GetConsoleOutputCP();
        }

        // Configure for VT output.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_OutputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        ChangeConsoleMode(m_OutputHandle.get(), newMode);
        m_SavedOutputMode = mode;
        if (m_restorePolicy == RestorePolicy::OnlyIfUnchanged)
        {
            m_ConfiguredOutputMode = TryGetConsoleMode(m_OutputHandle.get()).value_or(newMode);
        }
    }

    m_interactiveModeConfigured = true;
    cleanup.release();
}

ConsoleState::~ConsoleState()
{
    if (m_coordinationView)
    {
        try
        {
            ReleaseCoordination();
        }
        CATCH_LOG()

        LOG_IF_WIN32_BOOL_FALSE(UnmapViewOfFile(m_coordinationView));
    }

    RestoreConsoleState();
}

void ConsoleState::ConfigureInteractiveMode()
{
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));
    DWORD inputMode{};
    THROW_LAST_ERROR_IF(!GetConsoleMode(m_InputHandle.get(), &inputMode));
    WI_SetAllFlags(inputMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    WI_ClearAllFlags(inputMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    ChangeConsoleMode(m_InputHandle.get(), inputMode);

    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
    DWORD outputMode{};
    THROW_LAST_ERROR_IF(!GetConsoleMode(m_OutputHandle.get(), &outputMode));
    WI_SetAllFlags(outputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    ChangeConsoleMode(m_OutputHandle.get(), outputMode);
}

bool ConsoleState::AcquireCoordination()
{
    const auto window = GetConsoleWindow();
    if (!window || !m_InputHandle || !m_OutputHandle)
    {
        return false;
    }

    const auto name = std::format(L"Local\\WSL.ConsoleState.{:X}", reinterpret_cast<ULONG_PTR>(window));
    m_coordinationMutex.reset(CreateMutexW(nullptr, FALSE, (name + L".Mutex").c_str()));
    THROW_LAST_ERROR_IF(!m_coordinationMutex);
    m_coordinationMapping.reset(CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CoordinationState), (name + L".Mapping").c_str()));
    THROW_LAST_ERROR_IF(!m_coordinationMapping);
    auto view = MapViewOfFile(m_coordinationMapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CoordinationState));
    THROW_LAST_ERROR_IF(!view);
    auto unmap = wil::scope_exit([&] { LOG_IF_WIN32_BOOL_FALSE(UnmapViewOfFile(view)); });

    MutexLock lock(m_coordinationMutex.get());
    auto& state = *static_cast<CoordinationState*>(view);
    if (state.Version != 1)
    {
        state = {.Version = 1};
    }
    for (auto& owner : state.Owners)
    {
        if (owner.ProcessId && !IsOwnerAlive(owner))
        {
            owner = {};
        }
    }

    const auto pid = GetCurrentProcessId();
    auto owner = std::find_if(
        std::begin(state.Owners), std::end(state.Owners), [&](const auto& value) { return value.ProcessId == pid; });
    if (owner == std::end(state.Owners))
    {
        owner = std::find_if(std::begin(state.Owners), std::end(state.Owners), [](const auto& value) { return value.ProcessId == 0; });
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TOO_MANY_SESS), owner == std::end(state.Owners));
        *owner = {pid, 0};
    }

    const bool initialize = (owner->References == 0) && std::none_of(
        std::begin(state.Owners), std::end(state.Owners), [&](const auto& value) { return value.ProcessId && (&value != &*owner); });
    ++owner->References;
    if (initialize)
    {
        auto rollback = wil::scope_exit([&] { *owner = {}; });
        state.Baseline = CaptureConsoleValues(m_InputHandle.get(), m_OutputHandle.get());
        auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            ApplyConsoleValues(m_InputHandle.get(), m_OutputHandle.get(), state.Baseline);
        });
        ConfigureInteractiveMode();
        state.Configured = CaptureConsoleValues(m_InputHandle.get(), m_OutputHandle.get());
        restore.release();
        rollback.release();
    }

    m_coordinationView = view;
    unmap.release();
    return true;
}

void ConsoleState::ReleaseCoordination()
{
    MutexLock lock(m_coordinationMutex.get());
    auto& state = *static_cast<CoordinationState*>(m_coordinationView);
    const auto pid = GetCurrentProcessId();
    for (auto& owner : state.Owners)
    {
        if (owner.ProcessId == pid)
        {
            if (--owner.References == 0)
            {
                owner = {};
            }
        }
        else if (owner.ProcessId && !IsOwnerAlive(owner))
        {
            owner = {};
        }
    }

    if (std::none_of(std::begin(state.Owners), std::end(state.Owners), [](const auto& owner) { return owner.ProcessId != 0; }))
    {
        if (TryGetConsoleMode(m_InputHandle.get()) == state.Configured.InputMode)
        {
            TrySetConsoleMode(m_InputHandle.get(), state.Baseline.InputMode);
        }
        if (GetConsoleCP() == state.Configured.InputCodePage)
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(state.Baseline.InputCodePage));
        }
        if (TryGetConsoleMode(m_OutputHandle.get()) == state.Configured.OutputMode)
        {
            TrySetConsoleMode(m_OutputHandle.get(), state.Baseline.OutputMode);
        }
        if (GetConsoleOutputCP() == state.Configured.OutputCodePage)
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(state.Baseline.OutputCodePage));
        }
        state = {.Version = 1};
    }
}

void ConsoleState::RestoreConsoleState()
{
    if (m_InputHandle)
    {
        if (m_SavedInputCodePage.has_value())
        {
            const auto currentCodePage = GetConsoleCP();
            if ((m_restorePolicy == RestorePolicy::Always) || !m_ConfiguredInputCodePage.has_value() ||
                (currentCodePage == m_ConfiguredInputCodePage.value()))
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(m_SavedInputCodePage.value()));
            }

            m_SavedInputCodePage.reset();
            m_ConfiguredInputCodePage.reset();
        }

        if (m_SavedInputMode.has_value())
        {
            if (m_restorePolicy == RestorePolicy::Always)
            {
                TrySetConsoleMode(m_InputHandle.get(), m_SavedInputMode.value());
            }
            else
            {
                const auto currentMode = TryGetConsoleMode(m_InputHandle.get());
                if (!m_ConfiguredInputMode.has_value() || (currentMode == m_ConfiguredInputMode))
                {
                    TrySetConsoleMode(m_InputHandle.get(), m_SavedInputMode.value());
                }
            }

            m_SavedInputMode.reset();
            m_ConfiguredInputMode.reset();
        }
    }

    if (m_OutputHandle)
    {
        if (m_SavedOutputCodePage.has_value())
        {
            const auto currentCodePage = GetConsoleOutputCP();
            if ((m_restorePolicy == RestorePolicy::Always) || !m_ConfiguredOutputCodePage.has_value() ||
                (currentCodePage == m_ConfiguredOutputCodePage.value()))
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(m_SavedOutputCodePage.value()));
            }

            m_SavedOutputCodePage.reset();
            m_ConfiguredOutputCodePage.reset();
        }

        if (m_SavedOutputMode.has_value())
        {
            if (m_restorePolicy == RestorePolicy::Always)
            {
                TrySetConsoleMode(m_OutputHandle.get(), m_SavedOutputMode.value());
            }
            else
            {
                const auto currentMode = TryGetConsoleMode(m_OutputHandle.get());
                if (!m_ConfiguredOutputMode.has_value() || (currentMode == m_ConfiguredOutputMode))
                {
                    TrySetConsoleMode(m_OutputHandle.get(), m_SavedOutputMode.value());
                }
            }

            m_SavedOutputMode.reset();
            m_ConfiguredOutputMode.reset();
        }
    }
}

COORD ConsoleState::GetWindowSize() const
{
    if (m_OutputHandle)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX Info{};
        Info.cbSize = sizeof(Info);
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(m_OutputHandle.get(), &Info));
        return {
            static_cast<short>(Info.srWindow.Right - Info.srWindow.Left + 1),
            static_cast<short>(Info.srWindow.Bottom - Info.srWindow.Top + 1)};
    }

    LOG_HR_MSG(E_UNEXPECTED, "No console handle available for GetWindowSize");
    return {80, 24};
}

} // namespace wsl::windows::common
