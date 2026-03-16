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

// Default timeout constants for tests to prevent hanging
constexpr DWORD DefaultReadTimeoutMs = 5000;    // 5 seconds
constexpr DWORD DefaultWaitTimeoutMs = 30000;   // 30 seconds
constexpr DWORD DefaultMarkerTimeoutMs = 10000; // 10 seconds

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
    void Dump() const;
    void Verify(const WSLCExecutionResult& expected) const;
    std::vector<std::wstring> GetStdoutLines() const;
    std::wstring GetStdoutOneLine() const;
};

// Interactive session for testing wslc commands that require stdin/stdout interaction.
// - Background thread drains pipes to prevent deadlocks
// - Reads are served from background-drained buffers (no direct read fallback)
// - Full diagnostic output on failures
struct WSLCInteractiveSession
{
    WSLCInteractiveSession(std::wstring commandLine, wil::unique_hfile stdinWrite, wil::unique_hfile stdoutRead, wil::unique_hfile stderrRead, wil::unique_handle processHandle);

    void StopDraining();

    ~WSLCInteractiveSession();

    // Non-copyable, non-movable (has background thread)
    WSLCInteractiveSession(const WSLCInteractiveSession&) = delete;
    WSLCInteractiveSession& operator=(const WSLCInteractiveSession&) = delete;
    WSLCInteractiveSession(WSLCInteractiveSession&&) = delete;
    WSLCInteractiveSession& operator=(WSLCInteractiveSession&&) = delete;

    std::wstring CommandLine;

    void Write(const std::string& data);
    void Write(const std::wstring& data);
    void WriteLine(const std::string& line);
    void WriteLine(const std::wstring& line);
    void CloseStdin();

    std::string ReadStdout(DWORD timeoutMs = DefaultReadTimeoutMs);
    std::string ReadStderr(DWORD timeoutMs = DefaultReadTimeoutMs);
    std::string ReadUntil(const std::string& marker, DWORD timeoutMs = DefaultMarkerTimeoutMs);

    bool IsRunning() const;
    std::optional<int> GetExitCode() const;
    void WaitForExit(DWORD timeoutMs = DefaultWaitTimeoutMs);
    int Wait(DWORD timeoutMs = DefaultWaitTimeoutMs);
    bool Terminate(UINT exitCode = 1);
    void VerifyNoErrors();
    int Exit(DWORD timeoutMs = DefaultWaitTimeoutMs);
    int ExitAndVerifyNoErrors(DWORD timeoutMs = DefaultWaitTimeoutMs);

private:
    void DrainPipes(std::promise<void> ready);

    wil::unique_hfile m_stdinWrite;
    wil::unique_hfile m_stdoutRead;
    wil::unique_hfile m_stderrRead;
    wil::unique_handle m_processHandle;

    // Background draining for deadlock prevention
    wil::unique_event m_stopEvent;
    std::thread m_drainThread;
    std::string m_stdoutBuffer;
    std::string m_stderrBuffer;
    std::mutex m_stdoutMutex;
    std::mutex m_stderrMutex;
    wil::unique_event m_stdoutDataAvailable;
    wil::unique_event m_stderrDataAvailable;
};

WSLCExecutionResult RunWslc(const std::wstring& commandLine);
WSLCInteractiveSession RunWslcInteractive(const std::wstring& commandLine);
void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected);
std::wstring GetWslcHeader();

void WriteAndVerifyOutput(WSLCInteractiveSession& session, const std::string& command, const std::string& expectedOutput, DWORD timeoutMs = DefaultReadTimeoutMs);

} // namespace WSLCE2ETests
