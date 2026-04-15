/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutor.h

Abstract:

    This file contains the declaration of the WSLCExecutor class, which
    provides functionality to execute wslc commands and verify their results in
    end-to-end tests.
--*/

#pragma once

#include "precomp.h"
#include "windows/Common.h"

namespace WSLCE2ETests {

constexpr DWORD DefaultWaitTimeoutMs = 60000; // 60 seconds

enum class ElevationType
{
    Elevated,
    NonElevated
};

inline std::wstring GetWslcPath()
{
    return (std::filesystem::path(wsl::windows::common::wslutil::GetMsiPackagePath().value()) / L"wslc.exe").wstring();
}

struct WSLCExecutionResult
{
    std::wstring CommandLine{};
    std::optional<std::wstring> Stdout{};
    std::optional<std::wstring> Stderr{};
    std::optional<DWORD> ExitCode{};
    void Dump(bool escapeStrings = false) const;
    void Verify(const WSLCExecutionResult& expected) const;
    std::vector<std::wstring> GetStdoutLines() const;
    std::wstring GetStdoutOneLine() const;
    bool StdoutContainsLine(const std::wstring& expectedLine) const;
};

// Interactive session for testing wslc commands that require stdin/stdout interaction.
// Uses PartialHandleRead for race-free output validation
struct WSLCInteractiveSession
{
    WSLCInteractiveSession(
        std::wstring commandLine,
        wil::unique_hfile stdinWrite,
        wil::unique_hfile stdoutRead,
        wil::unique_hfile stderrRead,
        wil::unique_handle processHandle,
        wil::unique_handle nonElevatedToken = wil::unique_handle{});
    ~WSLCInteractiveSession();

    // Non-copyable, non-movable
    WSLCInteractiveSession(const WSLCInteractiveSession&) = delete;
    WSLCInteractiveSession& operator=(const WSLCInteractiveSession&) = delete;
    WSLCInteractiveSession(WSLCInteractiveSession&&) = delete;
    WSLCInteractiveSession& operator=(WSLCInteractiveSession&&) = delete;

    std::wstring CommandLine;

    void Write(const std::string& data);
    void WriteLine(const std::string& line);
    void ExpectStdout(const std::string& expected);
    void ExpectStderr(const std::string& expected);
    void ExpectCommandEcho(const std::string& command);

    bool IsRunning() const;
    void CloseStdin();
    std::optional<int> GetExitCode() const;
    void WaitForExit(DWORD timeoutMs = DefaultWaitTimeoutMs);
    int Wait(DWORD timeoutMs = DefaultWaitTimeoutMs);
    bool Terminate(UINT exitCode = 1);
    void VerifyNoErrors();
    int Exit(DWORD timeoutMs = DefaultWaitTimeoutMs);
    int ExitAndVerifyNoErrors(DWORD timeoutMs = DefaultWaitTimeoutMs);

private:
    wil::unique_hfile m_stdinWrite;
    wil::unique_hfile m_stdoutRead;
    wil::unique_hfile m_stderrRead;
    wil::unique_handle m_processHandle;
    wil::unique_handle m_nonElevatedToken; // Keep token alive for the lifetime of the session
    std::unique_ptr<PartialHandleRead> m_stdoutReader;
    std::unique_ptr<PartialHandleRead> m_stderrReader;
};

WSLCExecutionResult RunWslc(const std::wstring& commandLine, ElevationType elevationType = ElevationType::Elevated);
void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected, ElevationType elevationType = ElevationType::Elevated);

std::wstring GetWslcHeader();
WSLCInteractiveSession RunWslcInteractive(const std::wstring& commandLine, ElevationType elevationType = ElevationType::Elevated);

} // namespace WSLCE2ETests
