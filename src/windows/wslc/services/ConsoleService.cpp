/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleService.cpp

Abstract:

    This file contains the ConsoleService implementation

--*/
#include <precomp.h>
#include <WSLCProcessLauncher.h>
#include "ConsoleService.h"

namespace wsl::windows::wslc::services {

using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::io::MultiHandleWait;
using wsl::windows::common::io::OverlappedIOHandle;
using wsl::windows::common::io::ReadConsoleHandle;
using wsl::windows::common::io::ReadHandle;
using wsl::windows::common::io::RelayHandle;

namespace {

    // Interrupts and joins the stdin-relay worker thread at teardown.
    //
    // A worker thread is only created when stdin is not a character device (any non-FILE_TYPE_CHAR handle,
    // e.g. a redirected pipe); the console (FILE_TYPE_CHAR) and detach paths create none, so this no-ops
    // (not joinable) for them. When that handle is a non-overlapped (synchronous) pipe the worker blocks
    // in a ReadFile() that neither the exit event nor CancelIoEx() can interrupt, so once the relay's
    // scope-exit runs (normally after io.Run() returns) the join() below would hang until stdin is closed
    // -- the bug this guards against. The exit event is signaled first (enough for handles that support
    // overlapped IO), then CancelSynchronousIo() aborts the synchronous read the event cannot reach.
    void InterruptAndJoinInputThread(std::thread& inputThread, wil::unique_event& exitEvent)
    {
        if (!inputThread.joinable())
        {
            return;
        }

        if (exitEvent)
        {
            exitEvent.SetEvent();
        }

        // CancelSynchronousIo() only aborts I/O in flight on the target thread at the instant it is called,
        // so a single call can miss the worker while it is between read iterations. Cancel immediately to
        // unblock a worker already parked in ReadFile, then retry every 50ms until the thread exits;
        // WaitForSingleObject is both the termination test and the backoff. ERROR_NOT_FOUND means nothing
        // was in flight to cancel, which is expected, so only unexpected failures are logged.
        const auto threadHandle = static_cast<HANDLE>(inputThread.native_handle());
        do
        {
            if (!CancelSynchronousIo(threadHandle))
            {
                const auto lastError = GetLastError();
                LOG_HR_IF(HRESULT_FROM_WIN32(lastError), lastError != ERROR_NOT_FOUND);
            }
        } while (WaitForSingleObject(threadHandle, 50) == WAIT_TIMEOUT);

        inputThread.join();
    }

} // namespace

bool ConsoleService::RelayInteractiveTty(wsl::windows::common::ConsoleState& Console, ClientRunningWSLCProcess& Process, HANDLE Tty, bool TriggerRefresh)
{
    // Configure the console for interactive usage.
    Console.SetInteractiveMode();

    if (TriggerRefresh)
    {
        // In the case of an Attach, force a terminal resize to force the tty to refresh its display.
        // The docker client uses the same trick.

        auto size = Console.GetWindowSize();

        LOG_IF_FAILED(Process.Get().ResizeTty(size.Y + 1, size.X + 1));
        LOG_IF_FAILED(Process.Get().ResizeTty(size.Y, size.X));
    }

    wil::unique_event exitEvent;
    std::thread inputThread;

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { InterruptAndJoinInputThread(inputThread, exitEvent); });

    bool detached = false;
    MultiHandleWait io;

    auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);

    if (GetFileType(inputHandle) == FILE_TYPE_CHAR)
    {
        auto updateTerminal = [&Console, &Process]() {
            const auto windowSize = Console.GetWindowSize();
            LOG_IF_FAILED(Process.Get().ResizeTty(windowSize.Y, windowSize.X));
        };

        // TODO: Make this configurable (default to ctrl-p, ctrl-q).
        std::vector<char> detachSequence{0x10, 0x11};

        auto onDetach = [&detached]() { detached = true; };

        io.AddHandle(
            std::make_unique<RelayHandle<ReadConsoleHandle>>(inputHandle, Tty, std::move(updateTerminal), detachSequence, std::move(onDetach)),
            MultiHandleWait::NeedNotComplete);
    }
    else
    {
        exitEvent.create(wil::EventOptions::ManualReset);

        inputThread = std::thread{[&]() {
            try
            {
                windows::common::relay::InterruptableRelay(inputHandle, Tty, exitEvent.get());
            }
            CATCH_LOG();
        }};
    }

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(Tty, GetStdHandle(STD_OUTPUT_HANDLE)));

    io.Run({});

    return !detached;
}

void ConsoleService::RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr)
{
    windows::common::io::MultiHandleWait io;

    wil::unique_event exitEvent;
    std::thread inputThread;

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { InterruptAndJoinInputThread(inputThread, exitEvent); });

    if (Stdin.is_valid())
    {
        auto input = GetStdHandle(STD_INPUT_HANDLE);

        if (GetFileType(input) == FILE_TYPE_CHAR)
        {
            io.AddHandle(std::make_unique<RelayHandle<ReadConsoleHandle>>(input, std::move(Stdin)), MultiHandleWait::NeedNotComplete);
        }
        else
        {
            // Required because ReadFile() blocks if stdin doesn't support overlapped IO.
            // This can create pipe deadlocks if we get blocked reading stdin while data is available on stdout / stderr.
            // TODO: Will output CR instead of LF's which can confuse the linux app.
            // Consider a custom relay logic to fix this.
            exitEvent.create(wil::EventOptions::ManualReset);

            inputThread = std::thread{[&]() {
                try
                {
                    windows::common::relay::InterruptableRelay(GetStdHandle(STD_INPUT_HANDLE), Stdin.get(), exitEvent.get());
                }
                CATCH_LOG();

                Stdin.reset();
            }};
        }
    }

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stdout), GetStdHandle(STD_OUTPUT_HANDLE)));
    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stderr), GetStdHandle(STD_ERROR_HANDLE)));

    io.Run({});
}

int ConsoleService::AttachToCurrentConsole(wsl::windows::common::ConsoleState& console, wsl::windows::common::ClientRunningWSLCProcess&& process, bool triggerRefresh)
{
    if (WI_IsFlagSet(process.Flags(), WSLCProcessFlagsTty))
    {
        if (!RelayInteractiveTty(console, process, process.GetStdHandle(WSLCFDTty).get(), triggerRefresh))
        {
            windows::common::wslutil::PrintMessage(L"[detached]", stderr);
            return 0;
        }
    }
    else
    {
        wil::unique_handle stdinHandle;
        if (WI_IsFlagSet(process.Flags(), WSLCProcessFlagsStdin))
        {
            stdinHandle = process.GetStdHandle(WSLCFDStdin);
        }

        RelayNonTtyProcess(std::move(stdinHandle), process.GetStdHandle(WSLCFDStdout), process.GetStdHandle(WSLCFDStderr));
    }

    return process.Wait();
}
} // namespace wsl::windows::wslc::services
