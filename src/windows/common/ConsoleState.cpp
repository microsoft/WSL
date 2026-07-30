/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleState.cpp

Abstract:

    This file contains function definitions for the ConsoleState helper class.

--*/

#include "precomp.h"
#include "svccomm.hpp"
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
        LOG_LAST_ERROR_MSG("GetConsoleMode failed");
        return std::nullopt;
    }

    return mode;
}

} // namespace

namespace wsl::windows::common {

ConsoleState::ConsoleState()
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

    // Ensure console state is restored if this method throws.
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { RestoreConsoleState(); });

    if (m_InputHandle)
    {
        m_SavedInputCodePage = GetConsoleCP();
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));
        m_ConfiguredInputCodePage = GetConsoleCP();

        // Configure for raw input with VT support.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_InputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        WI_ClearAllFlags(newMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        ChangeConsoleMode(m_InputHandle.get(), newMode);
        m_SavedInputMode = mode;
        m_ConfiguredInputMode = TryGetConsoleMode(m_InputHandle.get()).value_or(newMode);
    }

    if (m_OutputHandle)
    {
        m_SavedOutputCodePage = GetConsoleOutputCP();
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
        m_ConfiguredOutputCodePage = GetConsoleOutputCP();

        // Configure for VT output.
        DWORD mode;
        THROW_LAST_ERROR_IF(!GetConsoleMode(m_OutputHandle.get(), &mode));

        DWORD newMode = mode;
        WI_SetAllFlags(newMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        ChangeConsoleMode(m_OutputHandle.get(), newMode);
        m_SavedOutputMode = mode;
        m_ConfiguredOutputMode = TryGetConsoleMode(m_OutputHandle.get()).value_or(newMode);
    }

    m_interactiveModeConfigured = true;
    cleanup.release();
}

ConsoleState::~ConsoleState()
{
    RestoreConsoleState();
}

void ConsoleState::RestoreConsoleState()
{
    if (m_InputHandle)
    {
        if (m_SavedInputCodePage.has_value())
        {
            const auto currentCodePage = GetConsoleCP();
            if (!m_ConfiguredInputCodePage.has_value() || (currentCodePage == m_ConfiguredInputCodePage.value()))
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(m_SavedInputCodePage.value()));
            }

            m_SavedInputCodePage.reset();
            m_ConfiguredInputCodePage.reset();
        }

        if (m_SavedInputMode.has_value())
        {
            const auto currentMode = TryGetConsoleMode(m_InputHandle.get());
            if (!m_ConfiguredInputMode.has_value() || (currentMode == m_ConfiguredInputMode))
            {
                TrySetConsoleMode(m_InputHandle.get(), m_SavedInputMode.value());
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
            if (!m_ConfiguredOutputCodePage.has_value() || (currentCodePage == m_ConfiguredOutputCodePage.value()))
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(m_SavedOutputCodePage.value()));
            }

            m_SavedOutputCodePage.reset();
            m_ConfiguredOutputCodePage.reset();
        }

        if (m_SavedOutputMode.has_value())
        {
            const auto currentMode = TryGetConsoleMode(m_OutputHandle.get());
            if (!m_ConfiguredOutputMode.has_value() || (currentMode == m_ConfiguredOutputMode))
            {
                TrySetConsoleMode(m_OutputHandle.get(), m_SavedOutputMode.value());
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
