/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SubProcess.h

Abstract:

    This file contains the SubProcess helper class definition.

--*/

#pragma once

#include <wil/resource.h>
#include <string>

namespace wsl::windows::common {

class SubProcess
{
public:
    struct ProcessOutput
    {
        DWORD ExitCode{};
        std::wstring Stdout;
        std::wstring Stderr;
    };

    SubProcess(LPCWSTR ApplicationName, LPCWSTR CommandLine, DWORD Flags = CREATE_UNICODE_ENVIRONMENT, DWORD StartupFlags = STARTF_FORCEOFFFEEDBACK);

    void SetStdHandles(HANDLE Stdin, HANDLE Stdout, HANDLE Stderr);
    void SetPseudoConsole(HPCON Console);
    void SetDesktopAppPolicy(DWORD Policy);
    void InheritHandle(HANDLE Handle);
    void SetEnvironment(LPVOID Environment);
    void SetWorkingDirectory(LPCWSTR Directory);
    void SetToken(HANDLE Token);
    void SetShowWindow(WORD Show);
    void SetFlags(DWORD Flag);

    wil::unique_handle Start();
    DWORD Run(DWORD Timeout = INFINITE);

    ProcessOutput RunAndCaptureOutput(DWORD Timeout = INFINITE, HANDLE StdErr = nullptr);

    static DWORD GetExitCode(HANDLE Process, DWORD Timeout = INFINITE);

private:
    helpers::unique_proc_attribute_list BuildProcessAttributes();

    LPCWSTR m_applicationName = nullptr;
    std::wstring m_commandLine;
    LPVOID m_environment = nullptr;
    LPCWSTR m_workingDirectory = nullptr;
    LPCWSTR m_desktop = nullptr;
    HANDLE m_token = nullptr;
    DWORD m_flags = 0;
    DWORD m_startupFlags = 0;

    HANDLE m_stdIn = nullptr;
    HANDLE m_stdOut = nullptr;
    HANDLE m_stdErr = nullptr;
    HPCON m_pseudoConsole = nullptr;
    std::optional<DWORD> m_desktopAppPolicy;
    std::optional<WORD> m_showWindow;
    std::vector<HANDLE> m_inheritHandles;
};

} // namespace wsl::windows::common