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
#include <algorithm>

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

namespace {
    // Internal helper function for reading from pipes
    std::string ReadFromPipe(HANDLE pipe, DWORD timeoutMs)
    {
        std::string result;
        char buffer[4096];

        auto startTime = GetTickCount64();
        while (GetTickCount64() - startTime < timeoutMs)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                break; // Pipe broken
            }

            if (available > 0)
            {
                DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
                DWORD read = 0;
                if (ReadFile(pipe, buffer, toRead, &read, nullptr) && read > 0)
                {
                    result.append(buffer, read);
                    startTime = GetTickCount64(); // Reset timeout after successful read
                }
                else
                {
                    break; // Read failed
                }
            }

            Sleep(10);
        }

        return result;
    }
} // anonymous namespace

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
    auto wslcPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"wslc.exe";
    auto cmd = L"\"" + wslcPath.wstring() + L"\" " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
    const auto output = process.RunAndCaptureOutput();
    return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = output.ExitCode};
}

WSLCInteractiveSession RunWslcInteractive(const std::wstring& commandLine)
{
    auto wslcPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"wslc.exe";
    auto cmd = L"\"" + wslcPath.wstring() + L"\" " + commandLine;
    Log::Comment(std::format(L"Starting interactive session: {}", cmd).c_str());

    // Create pipes with larger buffer to reduce blocking
    constexpr DWORD PIPE_BUFFER_SIZE = 65536; // 64KB
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    wil::unique_handle stdinRead, stdinWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdinRead, &stdinWrite, &sa, PIPE_BUFFER_SIZE));
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdinWrite.get(), HANDLE_FLAG_INHERIT, 0));

    wil::unique_handle stdoutRead, stdoutWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdoutRead, &stdoutWrite, &sa, PIPE_BUFFER_SIZE));
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0));

    wil::unique_handle stderrRead, stderrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stderrRead, &stderrWrite, &sa, PIPE_BUFFER_SIZE));
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0));

    auto process = std::make_unique<wsl::windows::common::SubProcess>(nullptr, cmd.c_str());
    process->SetStdHandles(stdinRead.get(), stdoutWrite.get(), stderrWrite.get());
    wil::unique_handle processHandle = process->Start();

    // Close child's ends immediately to prevent deadlocks
    stdinRead.reset();
    stdoutWrite.reset();
    stderrWrite.reset();

    // Verify process started
    DWORD exitCode = 0;
    if (GetExitCodeProcess(processHandle.get(), &exitCode) && exitCode != STILL_ACTIVE)
    {
        std::string errorOutput = ReadFromPipe(stderrRead.get(), 1000);
        THROW_HR_MSG(E_FAIL, "Process exited immediately with code %d. Stderr: %hs", exitCode, errorOutput.c_str());
    }

    return WSLCInteractiveSession(commandLine, std::move(stdinWrite), std::move(stdoutRead), std::move(stderrRead), std::move(processHandle));
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

void WriteAndVerifyOutput(WSLCInteractiveSession& session, const std::string& command, const std::string& expectedOutput, DWORD timeoutMs)
{
    session.WriteLine(command);
    auto output = session.ReadUntil(expectedOutput, timeoutMs);
    VERIFY_IS_TRUE(
        output.find(expectedOutput) != std::string::npos,
        std::format(L"Expected to find '{}' in output", wsl::shared::string::MultiByteToWide(expectedOutput)).c_str());
}

// WSLCInteractiveSession implementation

WSLCInteractiveSession::WSLCInteractiveSession(
    std::wstring commandLine, wil::unique_handle stdinWrite, wil::unique_handle stdoutRead, wil::unique_handle stderrRead, wil::unique_handle processHandle) :
    CommandLine(std::move(commandLine)),
    m_stdinWrite(std::move(stdinWrite)),
    m_stdoutRead(std::move(stdoutRead)),
    m_stderrRead(std::move(stderrRead)),
    m_processHandle(std::move(processHandle))
{
    // Start background thread to drain pipes and prevent deadlocks
    m_drainThread = std::thread([this]() { DrainPipes(); });
}

void WSLCInteractiveSession::StopDraining()
{
    // Only stop once - prevents double-close of handles
    bool expected = false;
    if (!m_drainingStopped.compare_exchange_strong(expected, true))
    {
        return;
    }

    m_stopDraining = true;
    m_stdoutRead.reset();
    m_stderrRead.reset();
}

WSLCInteractiveSession::~WSLCInteractiveSession()
{
    StopDraining();
    if (m_drainThread.joinable())
    {
        m_drainThread.join();
    }
}

void WSLCInteractiveSession::DrainPipes()
{
    char buffer[4096];
    while (!m_stopDraining)
    {
        // Note: There's a TOCTOU race between PeekNamedPipe and ReadFile where the pipe state can change
        // between calls. However, this thread is the only consumer of these pipes. The Windows kernel
        // guarantees that data in pipe buffers persists until read or the pipe closes. Since we're the
        // sole reader, data cannot disappear between Peek and Read due to context switches. If the pipe
        // closes between calls, ReadFile returns FALSE immediately (doesn't block). The append operation
        // is protected by a mutex while ReadFile executes outside the lock to avoid blocking. Deadlocks
        // are not possible with a single consumer and this locking strategy.

        // Drain stdout
        DWORD availableStdout = 0;
        if (PeekNamedPipe(m_stdoutRead.get(), nullptr, 0, nullptr, &availableStdout, nullptr) && availableStdout > 0)
        {
            DWORD read = 0;
            DWORD toRead = (std::min)(availableStdout, static_cast<DWORD>(sizeof(buffer)));
            if (ReadFile(m_stdoutRead.get(), buffer, toRead, &read, nullptr) && read > 0)
            {
                std::lock_guard<std::mutex> lock(m_stdoutMutex);
                m_stdoutBuffer.append(buffer, read);
            }
        }

        // Drain stderr
        DWORD availableStderr = 0;
        if (PeekNamedPipe(m_stderrRead.get(), nullptr, 0, nullptr, &availableStderr, nullptr) && availableStderr > 0)
        {
            DWORD read = 0;
            DWORD toRead = (std::min)(availableStderr, static_cast<DWORD>(sizeof(buffer)));
            if (ReadFile(m_stderrRead.get(), buffer, toRead, &read, nullptr) && read > 0)
            {
                std::lock_guard<std::mutex> lock(m_stderrMutex);
                m_stderrBuffer.append(buffer, read);
            }
        }

        Sleep(10);
    }
}

std::string WSLCInteractiveSession::ReadStdout(DWORD timeoutMs)
{
    auto startTime = GetTickCount64();

    // Only use background buffer - no race conditions
    while (GetTickCount64() - startTime < timeoutMs)
    {
        {
            std::lock_guard<std::mutex> lock(m_stdoutMutex);
            if (!m_stdoutBuffer.empty())
            {
                std::string result = std::move(m_stdoutBuffer);
                m_stdoutBuffer.clear();
                return result;
            }
        }
        Sleep(10);
    }

    return {};
}

std::string WSLCInteractiveSession::ReadStderr(DWORD timeoutMs)
{
    auto startTime = GetTickCount64();

    while (GetTickCount64() - startTime < timeoutMs)
    {
        {
            std::lock_guard<std::mutex> lock(m_stderrMutex);
            if (!m_stderrBuffer.empty())
            {
                std::string result = std::move(m_stderrBuffer);
                m_stderrBuffer.clear();
                return result;
            }
        }
        Sleep(10);
    }

    return {};
}

std::string WSLCInteractiveSession::ReadUntil(const std::string& marker, DWORD timeoutMs)
{
    std::string accumulated;
    auto startTime = GetTickCount64();

    while (GetTickCount64() - startTime < timeoutMs)
    {
        // Check background buffer (thread-safe)
        {
            std::lock_guard<std::mutex> lock(m_stdoutMutex);
            if (!m_stdoutBuffer.empty())
            {
                accumulated += m_stdoutBuffer;
                m_stdoutBuffer.clear();

                if (accumulated.find(marker) != std::string::npos)
                {
                    return accumulated;
                }
            }
        }

        // Don't do direct read - it can race with the drain thread
        // Just wait for the drain thread to populate the buffer
        Sleep(10);
    }

    THROW_HR_MSG(
        E_FAIL,
        "Timeout waiting for marker '%hs' after %dms. Got %zu bytes: %hs",
        marker.c_str(),
        timeoutMs,
        accumulated.size(),
        accumulated.c_str());
}

void WSLCInteractiveSession::Write(const std::string& data)
{
    DWORD written = 0;
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(m_stdinWrite.get(), data.c_str(), static_cast<DWORD>(data.size()), &written, nullptr));
    THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(m_stdinWrite.get()));
}

void WSLCInteractiveSession::Write(const std::wstring& data)
{
    Write(wsl::shared::string::WideToMultiByte(data));
}

void WSLCInteractiveSession::WriteLine(const std::string& line)
{
    Write(line + "\n");
}

void WSLCInteractiveSession::WriteLine(const std::wstring& line)
{
    Write(line + L"\n");
}

void WSLCInteractiveSession::CloseStdin()
{
    m_stdinWrite.reset();
}

bool WSLCInteractiveSession::IsRunning() const
{
    DWORD exitCode = 0;
    return GetExitCodeProcess(m_processHandle.get(), &exitCode) && exitCode == STILL_ACTIVE;
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
    auto ensureCleanup = wil::scope_exit([this] { StopDraining(); });

    auto result = WaitForSingleObject(m_processHandle.get(), timeoutMs);
    if (result == WAIT_TIMEOUT)
    {
        DWORD processId = GetProcessId(m_processHandle.get());

        // Collect diagnostics
        std::string remainingStdout, remainingStderr;
        try
        {
            remainingStdout = ReadStdout(500);
            remainingStderr = ReadStderr(500);
        }
        catch (...)
        {
        }

        Log::Warning(std::format(L"Process (PID: {}) did not exit within timeout of {}ms", processId, timeoutMs).c_str());
        if (!remainingStdout.empty())
        {
            Log::Warning(std::format(L"Remaining stdout: {}", wsl::shared::string::MultiByteToWide(remainingStdout)).c_str());
        }
        if (!remainingStderr.empty())
        {
            Log::Warning(std::format(L"Remaining stderr: {}", wsl::shared::string::MultiByteToWide(remainingStderr)).c_str());
        }

        Log::Warning(L"Attempting to terminate process forcefully");
        Terminate(999);
        WaitForSingleObject(m_processHandle.get(), 1000);

        THROW_HR_MSG(E_FAIL, "Process did not exit within timeout of %dms and was forcefully terminated", timeoutMs);
    }

    if (result != WAIT_OBJECT_0)
    {
        THROW_WIN32_MSG(result, "WaitForSingleObject failed with unexpected result");
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
    auto errors = ReadStderr(500);
    VERIFY_IS_TRUE(
        errors.empty(), std::format(L"Expected no errors on stderr, but got: {}", wsl::shared::string::MultiByteToWide(errors)).c_str());
}

int WSLCInteractiveSession::Exit(DWORD timeoutMs)
{
    WriteLine("exit");
    CloseStdin();
    return Wait(timeoutMs);
}

int WSLCInteractiveSession::ExitAndVerifyNoErrors(DWORD timeoutMs)
{
    VerifyNoErrors();
    return Exit(timeoutMs);
}

} // namespace WSLCE2ETests