/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SubProcess.cpp

Abstract:

    This file contains the subprocess helper class implementation.

--*/

#include "precomp.h"

#include "SubProcess.h"

using wsl::windows::common::SubProcess;

namespace {
wil::unique_file FileFromHandle(wil::unique_hfile& Handle, const char* Mode)
{
    using UniqueFd = wil::unique_any<int, decltype(_close), _close, wil::details::pointer_access_all, int, int, -1>;

    UniqueFd Fd(_open_osfhandle(reinterpret_cast<intptr_t>(Handle.get()), 0));
    THROW_LAST_ERROR_IF(Fd.get() < 0);

    Handle.release();

    wil::unique_file File(_fdopen(Fd.get(), Mode));
    THROW_LAST_ERROR_IF(!File);
    Fd.release();

    return File;
}

std::wstring ReadFileContent(wil::unique_hfile& Handle)
{
    THROW_LAST_ERROR_IF(SetFilePointer(Handle.get(), 0, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER);

    const auto File = FileFromHandle(Handle, "r");

    std::ifstream Stdout(File.get());
    return wsl::shared::string::MultiByteToWide(std::string(std::istreambuf_iterator<char>(Stdout), {}));
}
} // namespace

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
    //
    // Using pipes could cause a deadlock if the process writes more bytes
    // than the size of the pipe buffer. Using two files to prevent that.
    //

    using wsl::windows::common::filesystem::TempFile;
    const auto flags = filesystem::TempFileFlags::DeleteOnClose | filesystem::TempFileFlags::InheritHandle;
    auto stdoutFile = filesystem::TempFile(GENERIC_ALL, 0, OPEN_EXISTING, flags);
    m_stdOut = stdoutFile.Handle.get();

    std::optional<filesystem::TempFile> stderrFile;
    if (StdErr == nullptr)
    {
        stderrFile = filesystem::TempFile(GENERIC_ALL, 0, OPEN_EXISTING, flags);
    }

    m_stdErr = stderrFile ? stderrFile->Handle.get() : StdErr;

    const DWORD ExitCode = GetExitCode(Start().get(), Timeout);
    ProcessOutput output{ExitCode, ReadFileContent(stdoutFile.Handle), stderrFile ? ReadFileContent(stderrFile->Handle) : L""};

    // Clear out references to stdout and stderr temp files.
    m_stdOut = nullptr;
    m_stdErr = nullptr;
    return output;
}