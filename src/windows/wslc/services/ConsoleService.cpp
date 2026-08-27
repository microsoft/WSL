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
    // The worker only exists when stdin is not a character device (any non-FILE_TYPE_CHAR handle, e.g. a
    // redirected pipe), so this no-ops (not joinable) for a console. When stdin is a non-overlapped
    // (synchronous) pipe the worker blocks in a ReadFile() that neither the exit event nor CancelIoEx() can
    // interrupt, so the join() below would hang until stdin is closed -- the bug this guards against.
    void InterruptAndJoinInputThread(std::thread& inputThread, wil::unique_event& exitEvent)
    {
        if (!inputThread.joinable())
        {
            return;
        }

        WI_ASSERT(exitEvent);
        exitEvent.SetEvent();

        // Overlapped IO will get terminated by SetEvent(). Synchronous IO will not, so we need to cancel it.
        const auto threadHandle = static_cast<HANDLE>(inputThread.native_handle());
        DWORD waitResult = WAIT_TIMEOUT;
        while (waitResult == WAIT_TIMEOUT)
        {
            if (!CancelSynchronousIo(threadHandle))
            {
                // ERROR_NOT_FOUND means nothing to cancel; any other error is a corrupt handle that shouldn't happen.
                const auto cancelError = GetLastError();
                if (cancelError != ERROR_NOT_FOUND)
                {
                    FAIL_FAST_WIN32(cancelError);
                }
            }

            waitResult = WaitForSingleObject(threadHandle, 50);
        }

        // Anything but WAIT_OBJECT_0 (e.g. WAIT_FAILED) means a corrupt handle that shouldn't happen.
        FAIL_FAST_LAST_ERROR_IF(waitResult != WAIT_OBJECT_0);

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

int ConsoleService::AttachToCurrentConsole(
    Reporter& reporter, wsl::windows::common::ConsoleState& console, wsl::windows::common::ClientRunningWSLCProcess&& process, bool triggerRefresh)
{
    if (WI_IsFlagSet(process.Flags(), WSLCProcessFlagsTty))
    {
        if (!RelayInteractiveTty(console, process, process.GetStdHandle(WSLCFDTty).get(), triggerRefresh))
        {
            reporter.Info(L"[detached]\n");
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
