/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutor.cpp

Abstract:

    This file contains the implementation of the WSLCExecutor class, which is
    responsible for executing wslc commands and verifying their results in
    end-to-end tests.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

void WSLCExecutionResult::Dump() const
{
    Log::Comment((L"Command Line: \"" + CommandLine + L"\"").c_str());
    if (Stdout)
    {
        Log::Comment((L"Stdout: \"" + *Stdout + L"\"").c_str());
    }

    if (Stderr)
    {
        Log::Comment((L"Stderr: \"" + *Stderr + L"\"").c_str());
    }

    if (ExitCode)
    {
        Log::Comment((L"Exit Code: " + std::to_wstring(*ExitCode)).c_str());
    }
}

void WSLCExecutionResult::Verify(const WSLCExecutionResult& expected) const
{
    if (expected.Stdout)
    {
        VERIFY_ARE_EQUAL(*expected.Stdout, *Stdout);
    }

    if (expected.Stderr)
    {
        VERIFY_ARE_EQUAL(*expected.Stderr, *Stderr);
    }

    if (expected.ExitCode)
    {
        VERIFY_ARE_EQUAL(*expected.ExitCode, *ExitCode);
    }
}

std::vector<std::wstring> WSLCExecutionResult::GetStdoutLines() const
{
    std::vector<std::wstring> lines;
    std::wstringstream ss(*Stdout);
    std::wstring line;
    while (std::getline(ss, line))
    {
        // Remove carriage return if present
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        lines.push_back(line);
    }
    return lines;
}

std::wstring WSLCExecutionResult::GetStdoutOneLine() const
{
    auto stdoutLines = GetStdoutLines();
    VERIFY_ARE_EQUAL(1u, stdoutLines.size());
    return stdoutLines[0];
}

WSLCExecutionResult RunWslc(const std::wstring& commandLine)
{
    auto cmd = L"\"" + GetWslcPath() + L"\" " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
    const auto output = process.RunAndCaptureOutput();
    return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = output.ExitCode};
}

void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected)
{
    RunWslc(cmd).Verify(expected);
}

std::wstring GetWslcHeader()
{
    std::wstringstream header;
    header << L"Copyright (c) Microsoft Corporation. All rights reserved.\r\n"
           << L"For privacy information about this product please visit https://aka.ms/privacy.\r\n"
           << L"\r\n";
    return header.str();
}

WSLCInteractiveSession RunWslcInteractive(const std::wstring& commandLine)
{
    auto cmd = L"\"" + GetWslcPath() + L"\" " + commandLine;

    auto [childStdinRead, parentStdinWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(65536, false, true);
    auto [parentStdoutRead, childStdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(65536, true, false);
    auto [parentStderrRead, childStderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(65536, true, false);

    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(childStdinRead.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(childStdoutWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(childStderrWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
    process.SetStdHandles(childStdinRead.get(), childStdoutWrite.get(), childStderrWrite.get());
    wil::unique_handle processHandle = process.Start();

    childStdinRead.reset();
    childStdoutWrite.reset();
    childStderrWrite.reset();

    return WSLCInteractiveSession(
        commandLine, std::move(parentStdinWrite), std::move(parentStdoutRead), std::move(parentStderrRead), std::move(processHandle));
}

// WSLCInteractiveSession implementation

WSLCInteractiveSession::WSLCInteractiveSession(
    std::wstring commandLine, wil::unique_hfile stdinWrite, wil::unique_hfile stdoutRead, wil::unique_hfile stderrRead, wil::unique_handle processHandle) :
    CommandLine(std::move(commandLine)),
    m_stdinWrite(std::move(stdinWrite)),
    m_stdoutRead(std::move(stdoutRead)),
    m_stderrRead(std::move(stderrRead)),
    m_processHandle(std::move(processHandle))
{
    m_stdoutReader = std::make_unique<PartialHandleRead>(m_stdoutRead.get());
    m_stderrReader = std::make_unique<PartialHandleRead>(m_stderrRead.get());
}

void WSLCInteractiveSession::ExpectStdout(const std::string& expected)
{
    m_stdoutReader->ExpectConsume(expected);
}

void WSLCInteractiveSession::ExpectStderr(const std::string& expected)
{
    m_stderrReader->ExpectConsume(expected);
}

void WSLCInteractiveSession::WriteLineAndExpect(const std::string& line, const std::string& expectedOutput)
{
    WriteLine(line);
    ExpectStdout(expectedOutput);
}

void WSLCInteractiveSession::Write(const std::string& data)
{
    OVERLAPPED overlapped{};
    wil::unique_event event(wil::EventOptions::ManualReset);
    overlapped.hEvent = event.get();

    DWORD written = 0;
    if (!WriteFile(m_stdinWrite.get(), data.c_str(), static_cast<DWORD>(data.size()), &written, &overlapped))
    {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            THROW_LAST_ERROR_IF(WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0);
            THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(m_stdinWrite.get(), &overlapped, &written, FALSE));
        }
        else
        {
            THROW_WIN32(error);
        }
    }

    // Only flush if the handle is still valid (process might have closed it)
    if (m_stdinWrite.get() != nullptr)
    {
        if (!FlushFileBuffers(m_stdinWrite.get()))
        {
            DWORD error = GetLastError();

            // Ignore ERROR_BROKEN_PIPE - the process closed stdin, which is expected during shutdown
            if (error != ERROR_BROKEN_PIPE)
            {
                THROW_WIN32(error);
            }
        }
    }
}

void WSLCInteractiveSession::WriteLine(const std::string& line)
{
    Write(line + "\n");
}

bool WSLCInteractiveSession::IsRunning() const
{
    DWORD exitCode = 0;
    return GetExitCodeProcess(m_processHandle.get(), &exitCode) && exitCode == STILL_ACTIVE;
}

void WSLCInteractiveSession::CloseStdin()
{
    m_stdinWrite.reset();
}

std::optional<int> WSLCInteractiveSession::GetExitCode() const
{
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_processHandle.get(), &exitCode) && exitCode != STILL_ACTIVE)
    {
        return static_cast<int>(exitCode);
    }

    return std::nullopt;
}

void WSLCInteractiveSession::WaitForExit(DWORD timeoutMs)
{
    auto result = WaitForSingleObject(m_processHandle.get(), timeoutMs);
    if (result == WAIT_TIMEOUT)
    {
        DWORD processId = GetProcessId(m_processHandle.get());

        Log::Warning(std::format(L"Process (PID: {}) did not exit within timeout of {}ms", processId, timeoutMs).c_str());
        Log::Warning(L"Attempting to terminate process forcefully");
        Terminate(999);
        WaitForSingleObject(m_processHandle.get(), 1000);

        THROW_HR_MSG(E_FAIL, "Process did not exit within timeout of %dms and was forcefully terminated", timeoutMs);
    }

    if (result == WAIT_FAILED)
    {
        THROW_LAST_ERROR_MSG("WaitForSingleObject failed while waiting for process exit");
    }

    if (result != WAIT_OBJECT_0)
    {
        THROW_HR_MSG(E_UNEXPECTED, "WaitForSingleObject returned unexpected result: 0x%08lx", result);
    }
}

int WSLCInteractiveSession::Wait(DWORD timeoutMs)
{
    WaitForExit(timeoutMs);
    DWORD exitCode = 0;
    THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(m_processHandle.get(), &exitCode));
    return static_cast<int>(exitCode);
}

bool WSLCInteractiveSession::Terminate(UINT exitCode)
{
    return TerminateProcess(m_processHandle.get(), exitCode) != FALSE;
}

void WSLCInteractiveSession::VerifyNoErrors()
{
    m_stderrReader->ExpectClosed(DefaultWaitTimeoutMs);
}

int WSLCInteractiveSession::Exit(DWORD timeoutMs)
{
    WriteLine("exit");
    CloseStdin();
    return Wait(timeoutMs);
}

int WSLCInteractiveSession::ExitAndVerifyNoErrors(DWORD timeoutMs)
{
    const auto exitCode = Exit(timeoutMs);
    VerifyNoErrors();
    return exitCode;
}

} // namespace WSLCE2ETests