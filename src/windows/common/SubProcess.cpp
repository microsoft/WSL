/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SubProcess.cpp

Abstract:

    This file contains the subprocess helper class implementation.

--*/

#include "precomp.h"

#include "SubProcess.h"

using namespace wsl::windows::common::relay;
using wsl::windows::common::SubProcess;

SubProcess::SubProcess(LPCWSTR ApplicationName, LPCWSTR CommandLine, DWORD Flags, DWORD StartupFlags) :
    m_applicationName(ApplicationName), m_commandLine(CommandLine), m_flags(Flags), m_startupFlags(StartupFlags)
{
}

void SubProcess::SetStdHandles(HANDLE Stdin, HANDLE Stdout, HANDLE Stderr)
{
    m_stdIn = Stdin;
    m_stdOut = Stdout;
    m_stdErr = Stderr;
}

void SubProcess::InheritHandle(HANDLE Handle)
{
    // N.B. Trying to inherit the same handle twice will cause CreateProcess to fail with INVALID_ARG.
    if (std::find(m_inheritHandles.begin(), m_inheritHandles.end(), Handle) == m_inheritHandles.end())
    {
        m_inheritHandles.emplace_back(Handle);
    }
}

void SubProcess::SetPseudoConsole(HPCON Console)
{
    m_pseudoConsole = Console;
}

void SubProcess::SetDesktopAppPolicy(DWORD Policy)
{
    m_desktopAppPolicy = Policy;
}

void SubProcess::SetEnvironment(LPVOID Environment)
{
    m_environment = Environment;
}

void SubProcess::SetWorkingDirectory(LPCWSTR Directory)
{
    m_workingDirectory = Directory;
}

void SubProcess::SetFlags(DWORD Flag)
{
    WI_SetAllFlags(m_flags, Flag);
}

void SubProcess::SetToken(HANDLE Token)
{
    m_token = Token;
}

void SubProcess::SetShowWindow(WORD ShowWindow)
{
    m_showWindow = ShowWindow;
}

wsl::windows::common::helpers::unique_proc_attribute_list SubProcess::BuildProcessAttributes()
{
    DWORD attributes = 0;
    if (!m_inheritHandles.empty())
    {
        attributes++;
    }

    if (m_desktopAppPolicy.has_value())
    {
        attributes++;
    }

    if (m_pseudoConsole != nullptr)
    {
        attributes++;
    }

    if (attributes == 0)
    {
        return {};
    }

    auto list = helpers::CreateProcThreadAttributeList(attributes);

    // Handles to inherit
    // N.B. Pseudoconsoles can't be passed to PROC_THREAD_ATTRIBUTE_HANDLE_LIST
    // so if a pseudoconsole is passed, all handles need to be inherited.
    if (!m_inheritHandles.empty())
    {
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            list.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, m_inheritHandles.data(), m_inheritHandles.size() * sizeof(HANDLE), nullptr, nullptr));
    }

    // Desktop app policy
    if (m_desktopAppPolicy.has_value())
    {
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            list.get(), 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY, &m_desktopAppPolicy.value(), sizeof(m_desktopAppPolicy.value()), nullptr, nullptr));
    }

    // Pseudoconsole
    if (m_pseudoConsole != nullptr)
    {
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            list.get(), 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_pseudoConsole, sizeof(m_pseudoConsole), nullptr, nullptr));
    }

    return list;
}

wil::unique_handle SubProcess::Start()
{
    WI_SetFlag(m_flags, EXTENDED_STARTUPINFO_PRESENT);

    STARTUPINFOEX StartupInfo{};
    StartupInfo.StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES | m_startupFlags;

    // N.B. Passing a pseudoconsole requires all standard handles to be null
    if (m_pseudoConsole == nullptr)
    {
        StartupInfo.StartupInfo.hStdInput = ARGUMENT_PRESENT(m_stdIn) ? m_stdIn : GetStdHandle(STD_INPUT_HANDLE);
        StartupInfo.StartupInfo.hStdOutput = ARGUMENT_PRESENT(m_stdOut) ? m_stdOut : GetStdHandle(STD_OUTPUT_HANDLE);
        StartupInfo.StartupInfo.hStdError = ARGUMENT_PRESENT(m_stdErr) ? m_stdErr : GetStdHandle(STD_ERROR_HANDLE);

        if (StartupInfo.StartupInfo.hStdInput != nullptr)
        {
            InheritHandle(StartupInfo.StartupInfo.hStdInput);
        }

        if (StartupInfo.StartupInfo.hStdOutput != nullptr)
        {
            InheritHandle(StartupInfo.StartupInfo.hStdOutput);
        }

        if (StartupInfo.StartupInfo.hStdError != nullptr)
        {
            InheritHandle(StartupInfo.StartupInfo.hStdError);
        }
    }

    StartupInfo.StartupInfo.lpDesktop = const_cast<LPWSTR>(m_desktop);

    if (m_showWindow.has_value())
    {
        WI_SetFlag(StartupInfo.StartupInfo.dwFlags, STARTF_USESHOWWINDOW);
        StartupInfo.StartupInfo.wShowWindow = m_showWindow.value();
    }

    const auto attributes = BuildProcessAttributes();
    StartupInfo.lpAttributeList = attributes.get();

    wil::unique_process_information processInfo;
    THROW_IF_WIN32_BOOL_FALSE_MSG(
        CreateProcessAsUserW(
            m_token,
            m_applicationName,
            m_commandLine.data(),
            nullptr,
            nullptr,
            !m_inheritHandles.empty(),
            m_flags,
            m_environment,
            m_workingDirectory,
            &StartupInfo.StartupInfo,
            &processInfo),
        "ApplicationName: %ls, CommandLine: %ls, WorkingDirectory: %ls",
        m_applicationName,
        m_commandLine.c_str(),
        m_workingDirectory != nullptr ? m_workingDirectory : L"<null>");

    wil::unique_handle createdProcess{processInfo.hProcess};

    // Make sure that the process handle doesn't get closed on return
    processInfo.hProcess = nullptr;

    return createdProcess;
}

DWORD SubProcess::GetExitCode(HANDLE Process, DWORD Timeout)
{
    const auto status = WaitForSingleObject(Process, Timeout);
    THROW_HR_IF(HRESULT_FROM_NT(ERROR_TIMEOUT), status == WAIT_TIMEOUT);
    THROW_LAST_ERROR_IF(status != WAIT_OBJECT_0);

    DWORD exitCode{};
    THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(Process, &exitCode));
    return exitCode;
}

DWORD SubProcess::Run(DWORD Timeout)
{
    return GetExitCode(Start().get(), Timeout);
}

SubProcess::ProcessOutput SubProcess::RunAndCaptureOutput(DWORD Timeout, HANDLE StdErr)
{
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        // Clear out references to stdout and stderr pipes.
        m_stdOut = nullptr;
        m_stdErr = nullptr;
    });

    auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdoutWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

    m_stdOut = stdoutWrite.get();

    relay::MultiHandleWait io;
    std::string stdoutNative;
    std::string stderrNative;

    io.AddHandle(std::make_unique<relay::ReadHandle>(
        std::move(stdoutRead), [&](const gsl::span<char>& buffer) { stdoutNative.append(buffer.data(), buffer.size()); }));

    wil::unique_hfile stderrWrite;
    if (StdErr == nullptr)
    {
        wil::unique_hfile stderrRead;
        std::tie(stderrRead, stderrWrite) = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stderrWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

        m_stdErr = stderrWrite.get();

        io.AddHandle(std::make_unique<relay::ReadHandle>(
            std::move(stderrRead), [&](const gsl::span<char>& buffer) { stderrNative.append(buffer.data(), buffer.size()); }));
    }
    else
    {
        m_stdErr = StdErr;
    }

    auto process = Start();
    stdoutWrite.reset();
    stderrWrite.reset();

    io.Run(std::chrono::milliseconds{Timeout});

    // Reusing the same timeout since the std handles have been fully read at that point.
    const DWORD ExitCode = GetExitCode(process.get(), Timeout);
    ProcessOutput output{ExitCode, shared::string::MultiByteToWide(stdoutNative), shared::string::MultiByteToWide(stderrNative)};

    return output;
}