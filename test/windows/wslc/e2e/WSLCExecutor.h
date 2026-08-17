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
#include "VTSupport.h"

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
    bool StdoutContainsSubstring(const std::wstring& substring) const;
    bool StderrContainsSubstring(const std::wstring& substring) const;
};

struct PseudoConsole
{
    NON_COPYABLE(PseudoConsole);
    DEFAULT_MOVABLE(PseudoConsole);

    PseudoConsole(SHORT columns, SHORT rows);

    wil::unique_hfile InputWrite;
    wil::unique_hfile OutputRead;
    wsl::windows::common::helpers::unique_pseudo_console Handle;
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
        wil::unique_handle nonElevatedToken = wil::unique_handle{},
        wsl::windows::common::helpers::unique_pseudo_console pseudoConsole = {});
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

    // Convenience overloads for VT sequence helpers.
    void ExpectStdout(const wsl::windows::common::vt::Sequence& expected)
    {
        ExpectStdout(wsl::windows::common::string::WideToMultiByte(expected.Get()));
    }
    void ExpectStderr(const wsl::windows::common::vt::Sequence& expected)
    {
        ExpectStderr(wsl::windows::common::string::WideToMultiByte(expected.Get()));
    }

    void IgnoreSequence(const std::string& sequence);

    std::string GetStdoutData() const;

    void ResizePseudoConsole(SHORT columns, SHORT rows);

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
    wsl::windows::common::helpers::unique_pseudo_console m_pseudoConsole;
    wil::unique_handle m_processHandle;
    wil::unique_handle m_nonElevatedToken; // Keep token alive for the lifetime of the session
    std::unique_ptr<PartialHandleRead> m_stdoutReader;
    std::unique_ptr<PartialHandleRead> m_stderrReader;
    std::optional<std::string> m_ignoreSequence;
};

WSLCExecutionResult RunWslc(const std::wstring& commandLine, ElevationType elevationType = ElevationType::Elevated, HANDLE stdinHandle = nullptr);
WSLCExecutionResult RunWslcAndRedirectToFile(
    const std::wstring& commandLine,
    std::optional<std::filesystem::path> outputPath = std::nullopt,
    ElevationType elevationType = ElevationType::Elevated);
WSLCExecutionResult RunWslcWithStdinFile(
    const std::wstring& commandLine, const std::filesystem::path& stdinFilePath, ElevationType elevationType = ElevationType::Elevated);
void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected, ElevationType elevationType = ElevationType::Elevated);

WSLCInteractiveSession RunWslcInteractive(
    const std::wstring& commandLine, ElevationType elevationType = ElevationType::Elevated, std::optional<PseudoConsole> pseudoConsole = std::nullopt);

} // namespace WSLCE2ETests
