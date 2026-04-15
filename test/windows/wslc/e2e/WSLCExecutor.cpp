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
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

namespace {
    wil::unique_handle GetNonElevatedPrimaryToken()
    {
        // This method is necessary because GetNonElevatedToken(TokenPrimary) does
        // not actually give a de-elevated token when called from an elevated process.
        // By getting impersonation token first this de-elevates the token, and then
        // converts it to a primary token.
        auto impersonationToken = GetNonElevatedToken(TokenImpersonation);
        wil::unique_handle primaryToken;
        THROW_IF_WIN32_BOOL_FALSE(
            DuplicateTokenEx(impersonationToken.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken));

        VERIFY_IS_FALSE(wsl::windows::common::security::IsTokenElevated(primaryToken.get()));
        return primaryToken;
    }
} // namespace

void WSLCExecutionResult::Dump(bool escapeStrings) const
{
    Log::Comment((L"Command Line: \"" + CommandLine + L"\"").c_str());
    if (Stdout)
    {
        if (escapeStrings)
        {
            std::string stdoutStr = wsl::windows::common::string::WideToMultiByte(*Stdout);
            std::string escapedStdout = EscapeString(stdoutStr);
            Log::Comment(std::format(L"Stdout: \"{}\"", wsl::shared::string::MultiByteToWide(escapedStdout)).c_str());
        }
        else
        {
            Log::Comment((L"Stdout: \"" + *Stdout + L"\"").c_str());
        }
    }

    if (Stderr)
    {
        if (escapeStrings)
        {
            std::string stderrStr = wsl::windows::common::string::WideToMultiByte(*Stderr);
            std::string escapedStderr = EscapeString(stderrStr);
            Log::Comment(std::format(L"Stderr (escaped): \"{}\"", wsl::shared::string::MultiByteToWide(escapedStderr)).c_str());
        }
        else
        {
            Log::Comment((L"Stderr: \"" + *Stderr + L"\"").c_str());
        }
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

    // Remove empty trailing lines (common when output ends with \n)
    while (!stdoutLines.empty() && stdoutLines.back().empty())
    {
        stdoutLines.pop_back();
    }

    VERIFY_ARE_EQUAL(1u, stdoutLines.size());
    return stdoutLines[0];
}

bool WSLCExecutionResult::StdoutContainsLine(const std::wstring& expectedLine) const
{
    VERIFY_IS_TRUE(Stdout.has_value());
    for (const auto& line : GetStdoutLines())
    {
        if (line == expectedLine)
        {
            return true;
        }
    }

    return false;
}

WSLCExecutionResult RunWslc(const std::wstring& commandLine, ElevationType elevationType)
{
    auto cmd = L"\"" + GetWslcPath() + L"\" " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());

    // If running non-elevated we need to keep the token alive until it completes.
    wil::unique_handle nonElevatedToken;
    if (elevationType == ElevationType::NonElevated)
    {
        nonElevatedToken = GetNonElevatedPrimaryToken();
        process.SetToken(nonElevatedToken.get());
    }

    const auto output = process.RunAndCaptureOutput();
    return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = output.ExitCode};
}

void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected, ElevationType elevationType)
{
    RunWslc(cmd, elevationType).Verify(expected);
}

WSLCExecutionResult RunWslcAndRedirectToFile(const std::wstring& commandLine, std::optional<std::filesystem::path> outputPath, ElevationType elevationType)
{
    auto cmd = L"\"" + GetWslcPath() + L"\" " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());

    // If running non-elevated we need to keep the token alive until it completes.
    wil::unique_handle nonElevatedToken;
    if (elevationType == ElevationType::NonElevated)
    {
        nonElevatedToken = GetNonElevatedPrimaryToken();
        process.SetToken(nonElevatedToken.get());
    }

    auto [parentStderrRead, childStderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(childStderrWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

    wil::unique_hfile redirectedStdout;
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    std::wstring effectiveCommandLine = commandLine;
    if (outputPath.has_value())
    {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        redirectedStdout.reset(CreateFileW(
            outputPath->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &securityAttributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!redirectedStdout);
        stdoutHandle = redirectedStdout.get();
        effectiveCommandLine = std::format(L"{} > {}", commandLine, outputPath->wstring());
    }

    process.SetStdHandles(nullptr, stdoutHandle, childStderrWrite.get());

    const auto processHandle = process.Start();
    childStderrWrite.reset();

    const auto exitCode = wsl::windows::common::SubProcess::GetExitCode(processHandle.get());
    const auto stdErrOutput = wsl::shared::string::MultiByteToWide(ReadToString(parentStderrRead.get()));

    return {.CommandLine = std::move(effectiveCommandLine), .Stdout = L"", .Stderr = stdErrOutput, .ExitCode = exitCode};
}

std::wstring GetWslcHeader()
{
    std::wstringstream header;
    header << L"Copyright (c) Microsoft Corporation. All rights reserved.\r\n"
           << L"For privacy information about this product please visit https://aka.ms/privacy.\r\n"
           << L"\r\n";
    return header.str();
}

WSLCInteractiveSession RunWslcInteractive(const std::wstring& commandLine, ElevationType elevationType)
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

    wil::unique_handle nonElevatedToken;
    if (elevationType == ElevationType::NonElevated)
    {
        nonElevatedToken = GetNonElevatedPrimaryToken();
        process.SetToken(nonElevatedToken.get());
    }

    wil::unique_handle processHandle = process.Start();

    childStdinRead.reset();
    childStdoutWrite.reset();
    childStderrWrite.reset();

    return WSLCInteractiveSession(
        commandLine,
        std::move(parentStdinWrite),
        std::move(parentStdoutRead),
        std::move(parentStderrRead),
        std::move(processHandle),
        std::move(nonElevatedToken)); // Transfer token ownership to the session
}

// WSLCInteractiveSession implementation

WSLCInteractiveSession::WSLCInteractiveSession(
    std::wstring commandLine,
    wil::unique_hfile stdinWrite,
    wil::unique_hfile stdoutRead,
    wil::unique_hfile stderrRead,
    wil::unique_handle processHandle,
    wil::unique_handle nonElevatedToken) :
    CommandLine(std::move(commandLine)),
    m_stdinWrite(std::move(stdinWrite)),
    m_stdoutRead(std::move(stdoutRead)),
    m_stderrRead(std::move(stderrRead)),
    m_processHandle(std::move(processHandle)),
    m_nonElevatedToken(std::move(nonElevatedToken))
{
    m_stdoutReader = std::make_unique<PartialHandleRead>(m_stdoutRead.get());
    m_stderrReader = std::make_unique<PartialHandleRead>(m_stderrRead.get());
}

WSLCInteractiveSession::~WSLCInteractiveSession()
{
    // Best-effort cleanup to avoid orphaned wslc process if Exit()/Wait() were not called.
    if (!m_processHandle.is_valid())
    {
        return;
    }

    CloseStdin();

    DWORD waitResult = ::WaitForSingleObject(m_processHandle.get(), DefaultWaitTimeoutMs);
    if (waitResult == WAIT_TIMEOUT)
    {
        // Still running: terminate and wait again, but do not throw.
        ::TerminateProcess(m_processHandle.get(), 1);
        ::WaitForSingleObject(m_processHandle.get(), DefaultWaitTimeoutMs);
    }
}

void WSLCInteractiveSession::ExpectStdout(const std::string& expected)
{
    Log::Comment(std::format(L"Expecting stdout: \"{}\"", wsl::shared::string::MultiByteToWide(EscapeString(expected))).c_str());
    m_stdoutReader->ExpectConsume(expected);
}

void WSLCInteractiveSession::ExpectStderr(const std::string& expected)
{
    Log::Comment(std::format(L"Expecting stderr: \"{}\"", wsl::shared::string::MultiByteToWide(EscapeString(expected))).c_str());
    m_stderrReader->ExpectConsume(expected);
}

void WSLCInteractiveSession::ExpectCommandEcho(const std::string& command)
{
    // TTY mode: expect command echo, then B_END and carriage return
    ExpectStdout(std::format("{}\r\n{}\r", command, VT::B_END));
}

void WSLCInteractiveSession::Write(const std::string& data)
{
    Log::Comment(std::format(L"Writing to stdin: \"{}\"", wsl::shared::string::MultiByteToWide(EscapeString(data))).c_str());

    OVERLAPPED overlapped{};
    wil::unique_event event(wil::EventOptions::ManualReset);
    overlapped.hEvent = event.get();

    DWORD written = 0;
    if (!WriteFile(m_stdinWrite.get(), data.c_str(), static_cast<DWORD>(data.size()), &written, &overlapped))
    {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            DWORD waitResult = WaitForSingleObject(event.get(), DefaultWaitTimeoutMs);
            if (waitResult == WAIT_TIMEOUT)
            {
                THROW_HR(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            }
            else if (waitResult == WAIT_FAILED)
            {
                THROW_LAST_ERROR();
            }
            else if (waitResult != WAIT_OBJECT_0)
            {
                THROW_HR_MSG(E_UNEXPECTED, "WaitForSingleObject returned unexpected result: 0x%08lx", waitResult);
            }

            THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(m_stdinWrite.get(), &overlapped, &written, FALSE));
        }
        else
        {
            THROW_WIN32(error);
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
        WaitForSingleObject(m_processHandle.get(), DefaultWaitTimeoutMs);

        THROW_HR_MSG(E_FAIL, "Process did not exit within timeout of %lums and was forcefully terminated", timeoutMs);
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

    // Verify that stderr was actually empty - not just closed
    const auto& stderrContent = m_stderrReader->GetData();
    if (!stderrContent.empty())
    {
        VERIFY_FAIL(std::format(L"Expected no errors but stderr contained: {}", wsl::shared::string::MultiByteToWide(EscapeString(stderrContent)))
                        .c_str());
    }
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