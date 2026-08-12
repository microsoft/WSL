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

struct ConsoleValues
{
    DWORD InputMode;
    UINT InputCodePage;
    DWORD OutputMode;
    UINT OutputCodePage;
};

struct CoordinationState
{
    ConsoleValues Baseline;
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

std::optional<ULONG> TryGetConsoleId()
{
    const auto console = NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters->Reserved2[0];
    if (!console || (console == INVALID_HANDLE_VALUE))
    {
        return std::nullopt;
    }

    HANDLE serverPid{};
    DWORD bytesReturned{};
    if (!DeviceIoControl(console, IOCTL_CONDRV_GET_SERVER_PID, nullptr, 0, &serverPid, sizeof(serverPid), &bytesReturned, nullptr))
    {
        LOG_LAST_ERROR_MSG("IOCTL_CONDRV_GET_SERVER_PID failed");
        return std::nullopt;
    }

    return HandleToUlong(serverPid);
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

    if (m_restorePolicy == RestorePolicy::Cooperative)
    {
        if (m_InputHandle || m_OutputHandle)
        {
            THROW_HR_IF(E_UNEXPECTED, !m_InputHandle || !m_OutputHandle);
            THROW_HR_IF(E_UNEXPECTED, m_SavedOutputCodePage.has_value());
            AcquireCoordination();
        }

        m_interactiveModeConfigured = true;
        return;
    }

    // Ensure console state is restored if this method throws.
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { RestoreConsoleState(); });

    if (m_InputHandle)
    {
        m_SavedInputCodePage = GetConsoleCP();
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));

        // Configure for raw input with VT support.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_InputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        WI_ClearAllFlags(newMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        ChangeConsoleMode(m_InputHandle.get(), newMode);
        m_SavedInputMode = mode;
    }

    if (m_OutputHandle)
    {
        if (!m_SavedOutputCodePage.has_value())
        {
            m_SavedOutputCodePage = GetConsoleOutputCP();
        }

        LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));

        // Configure for VT output.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_OutputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        ChangeConsoleMode(m_OutputHandle.get(), newMode);
        m_SavedOutputMode = mode;
    }

    m_interactiveModeConfigured = true;
    cleanup.release();
}

void ConsoleState::SetOutputCodePageUtf8()
{
    if (!m_OutputHandle)
    {
        return;
    }

    if (!m_SavedOutputCodePage.has_value())
    {
        m_SavedOutputCodePage = GetConsoleOutputCP();
    }

    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
}

ConsoleState::~ConsoleState()
{
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

void ConsoleState::AcquireCoordination()
{
    const auto consoleId = TryGetConsoleId();
    THROW_HR_IF(E_UNEXPECTED, !consoleId);

    auto token = wil::open_current_access_token();
    const auto tokenUser = wil::get_token_information<TOKEN_USER>(token.get());
    const auto userSid = wslutil::SidToString(tokenUser->User.Sid);
    const auto sddl = std::format(L"D:P(A;;GA;;;SY)(A;;GA;;;{})S:(ML;;NW;;;ME)", userSid.get());
    PSECURITY_DESCRIPTOR securityDescriptor{};
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &securityDescriptor, nullptr));
    m_coordinationSecurityDescriptor.reset(securityDescriptor);
    m_coordinationSecurityAttributes = {sizeof(m_coordinationSecurityAttributes), m_coordinationSecurityDescriptor.get(), FALSE};

    m_coordinationName = std::format(L"Local\\WSL.ConsoleState.v1.{}.{:X}", userSid.get(), consoleId.value());
    m_coordinationMutex.reset(CreateMutexExW(
        &m_coordinationSecurityAttributes, (m_coordinationName + L".Mutex").c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    THROW_LAST_ERROR_IF(!m_coordinationMutex);

    MutexLock lock(m_coordinationMutex.get());

    SetLastError(ERROR_SUCCESS);
    wil::unique_handle event{CreateEventExW(
        &m_coordinationSecurityAttributes, (m_coordinationName + L".Event").c_str(), CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE)};
    THROW_LAST_ERROR_IF(!event);
    const bool initialize = GetLastError() != ERROR_ALREADY_EXISTS;

    SetLastError(ERROR_SUCCESS);
    wil::unique_handle mapping{CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &m_coordinationSecurityAttributes,
        PAGE_READWRITE,
        0,
        sizeof(CoordinationState),
        (m_coordinationName + L".Mapping").c_str())};
    THROW_LAST_ERROR_IF(!mapping);
    const bool mappingCreated = GetLastError() != ERROR_ALREADY_EXISTS;
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), initialize != mappingCreated);

    auto view = MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CoordinationState));
    THROW_LAST_ERROR_IF(!view);
    auto unmapView = wil::scope_exit([&] { LOG_IF_WIN32_BOOL_FALSE(UnmapViewOfFile(view)); });

    if (initialize)
    {
        auto& baseline = static_cast<CoordinationState*>(view)->Baseline;
        baseline = CaptureConsoleValues(m_InputHandle.get(), m_OutputHandle.get());
        auto restore = wil::scope_exit_log(
            WI_DIAGNOSTICS_INFO, [&] { ApplyConsoleValues(m_InputHandle.get(), m_OutputHandle.get(), baseline); });
        ConfigureInteractiveMode();
        restore.release();
    }

    m_coordinationEvent = std::move(event);
    m_coordinationMapping = std::move(mapping);
    m_coordinationView = view;
    unmapView.release();
}

void ConsoleState::RestoreConsoleState() noexcept
{
    if (m_coordinationView)
    {
        try
        {
            MutexLock lock(m_coordinationMutex.get());

            // The mutex prevents a new first participant from creating its event until the previous
            // last participant has restored the baseline and released all epoch objects.
            m_coordinationEvent.reset();
            SetLastError(ERROR_SUCCESS);
            wil::unique_handle probe{CreateEventExW(
                &m_coordinationSecurityAttributes, (m_coordinationName + L".Event").c_str(), CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE)};
            THROW_LAST_ERROR_IF(!probe);
            const bool lastParticipant = GetLastError() != ERROR_ALREADY_EXISTS;
            probe.reset();

            if (lastParticipant)
            {
                ApplyConsoleValues(m_InputHandle.get(), m_OutputHandle.get(), static_cast<CoordinationState*>(m_coordinationView)->Baseline);
            }

            LOG_IF_WIN32_BOOL_FALSE(UnmapViewOfFile(m_coordinationView));
            m_coordinationView = nullptr;
            m_coordinationMapping.reset();
        }
        CATCH_LOG()

        if (m_coordinationView)
        {
            LOG_IF_WIN32_BOOL_FALSE(UnmapViewOfFile(m_coordinationView));
            m_coordinationView = nullptr;
        }

        m_coordinationEvent.reset();
        m_coordinationMapping.reset();
        m_coordinationMutex.reset();
        m_coordinationSecurityAttributes = {};
        m_coordinationSecurityDescriptor.reset();
        m_coordinationName.clear();
        return;
    }

    if (m_InputHandle)
    {
        if (m_SavedInputCodePage.has_value())
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(m_SavedInputCodePage.value()));
            m_SavedInputCodePage.reset();
        }

        if (m_SavedInputMode.has_value())
        {
            TrySetConsoleMode(m_InputHandle.get(), m_SavedInputMode.value());
            m_SavedInputMode.reset();
        }
    }

    if (m_OutputHandle)
    {
        if (m_SavedOutputCodePage.has_value())
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(m_SavedOutputCodePage.value()));
            m_SavedOutputCodePage.reset();
        }

        if (m_SavedOutputMode.has_value())
        {
            TrySetConsoleMode(m_OutputHandle.get(), m_SavedOutputMode.value());
            m_SavedOutputMode.reset();
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
