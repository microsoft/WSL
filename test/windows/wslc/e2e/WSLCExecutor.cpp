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
    std::wstring commandLine, wil::unique_hfile stdinWrite, wil::unique_hfile stdoutRead, wil::unique_hfile stderrRead, wil::unique_handle processHandle) :
    CommandLine(std::move(commandLine)),
    m_stdinWrite(std::move(stdinWrite)),
    m_stdoutRead(std::move(stdoutRead)),
    m_stderrRead(std::move(stderrRead)),
    m_processHandle(std::move(processHandle)),
    m_stopEvent(wil::EventOptions::ManualReset),
    m_stdoutDataAvailable(wil::EventOptions::ManualReset),
    m_stderrDataAvailable(wil::EventOptions::ManualReset)
{
    std::promise<void> drainThreadReady;
    auto drainThreadReadyFuture = drainThreadReady.get_future();

    m_drainThread = std::thread([this, ready = std::move(drainThreadReady)]() mutable { DrainPipes(std::move(ready)); });

    // Wait for drain thread to be fully initialized with timeout
    auto waitResult = drainThreadReadyFuture.wait_for(std::chrono::milliseconds(DefaultWaitTimeoutMs));
    if (waitResult == std::future_status::timeout)
    {
        m_stopEvent.SetEvent();
        if (m_drainThread.joinable())
        {
            m_drainThread.join();
        }
        THROW_HR_MSG(E_FAIL, "Drain thread failed to initialize within %dms timeout", DefaultWaitTimeoutMs);
    }
}

void WSLCInteractiveSession::DrainPipes(std::promise<void> ready)
{
    using namespace wsl::windows::common::relay;

    // Track whether we've fulfilled the promise to prevent hangs in constructor
    bool promiseFulfilled = false;

    // Ensure promise is always fulfilled to prevent hangs in constructor
    auto ensurePromiseFulfilled = wil::scope_exit([&ready, &promiseFulfilled]() {
        if (!promiseFulfilled)
        {
            // If we haven't set the value yet, set it now (even on error path)
            try
            {
                ready.set_value();
            }
            catch (...)
            {
                // Promise was already fulfilled (shouldn't happen with our logic)
            }
        }
    });

    try
    {
        MultiHandleWait io;

        auto stdoutCallback = [this](const gsl::span<char>& data) {
            std::lock_guard<std::mutex> lock(m_stdoutMutex);
            m_stdoutBuffer.append(data.data(), data.size());
            m_stdoutDataAvailable.SetEvent();
        };

        io.AddHandle(std::make_unique<ReadHandle>(HandleWrapper{std::move(m_stdoutRead)}, std::move(stdoutCallback)));

        auto stderrCallback = [this](const gsl::span<char>& data) {
            std::lock_guard<std::mutex> lock(m_stderrMutex);
            m_stderrBuffer.append(data.data(), data.size());
            m_stderrDataAvailable.SetEvent();
        };

        io.AddHandle(std::make_unique<ReadHandle>(HandleWrapper{std::move(m_stderrRead)}, std::move(stderrCallback)));

        // Add stop event to cancel the wait loop
        io.AddHandle(std::make_unique<EventHandle>(HandleWrapper{m_stopEvent.get()}), MultiHandleWait::CancelOnCompleted);

        // Signal that initialization is complete
        ready.set_value();
        promiseFulfilled = true;

        // Run indefinitely until m_stopEvent is signaled or pipes are closed
        // Will throw ERROR_CANCELLED when stop event is signaled
        io.Run({});
    }
    catch (const wil::ResultException& ex)
    {
        // ERROR_CANCELLED means m_stopEvent was signaled - this is expected
        if (ex.GetErrorCode() != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            Log::Error(std::format(
                           L"Drain thread encountered unexpected error: 0x{:08X} - {}",
                           ex.GetErrorCode(),
                           wsl::shared::string::MultiByteToWide(ex.what()))
                           .c_str());
        }
    }
    catch (...)
    {
        Log::Error(L"Drain thread encountered unknown exception");
    }
}

void WSLCInteractiveSession::StopDraining()
{
    m_stopEvent.SetEvent();
    if (m_drainThread.joinable())
    {
        m_drainThread.join();
    }

    // Wake up any waiting readers
    m_stdoutDataAvailable.SetEvent();
    m_stderrDataAvailable.SetEvent();
}

WSLCInteractiveSession::~WSLCInteractiveSession()
{
    StopDraining();
}

std::string WSLCInteractiveSession::ReadStdout(DWORD timeoutMs)
{
    auto startTime = GetTickCount64();

    while (GetTickCount64() - startTime < timeoutMs)
    {
        {
            std::lock_guard<std::mutex> lock(m_stdoutMutex);
            if (!m_stdoutBuffer.empty())
            {
                std::string result = std::move(m_stdoutBuffer);
                m_stdoutBuffer.clear();
                m_stdoutDataAvailable.ResetEvent();
                return result;
            }
        }

        // Wait for data to be available with remaining timeout
        DWORD remainingTimeout = timeoutMs - static_cast<DWORD>(GetTickCount64() - startTime);
        if (WaitForSingleObject(m_stdoutDataAvailable.get(), remainingTimeout) == WAIT_TIMEOUT)
        {
            break;
        }
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
                m_stderrDataAvailable.ResetEvent();
                return result;
            }
        }

        // Wait for data to be available with remaining timeout
        DWORD remainingTimeout = timeoutMs - static_cast<DWORD>(GetTickCount64() - startTime);
        if (WaitForSingleObject(m_stderrDataAvailable.get(), remainingTimeout) == WAIT_TIMEOUT)
        {
            break;
        }
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
                    m_stdoutDataAvailable.ResetEvent();
                    return accumulated;
                }

                // Marker not found and buffer empty, reset event to wait for more data.
                if (m_stdoutBuffer.empty())
                {
                    m_stdoutDataAvailable.ResetEvent();
                }
            }
        }

        // Wait for more data with remaining timeout
        DWORD remainingTimeout = timeoutMs - static_cast<DWORD>(GetTickCount64() - startTime);
        if (WaitForSingleObject(m_stdoutDataAvailable.get(), remainingTimeout) == WAIT_TIMEOUT)
        {
            break;
        }
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

    if (result == WAIT_FAILED)
    {
        // Use the last error from WaitForSingleObject.
        THROW_LAST_ERROR_MSG("WaitForSingleObject failed while waiting for process exit");
    }

    if (result != WAIT_OBJECT_0)
    {
        // For process handles, any other result is unexpected and indicates a logic error.
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
    const auto exitCode = Exit(timeoutMs);
    VerifyNoErrors();
    return exitCode;
}

} // namespace WSLCE2ETests