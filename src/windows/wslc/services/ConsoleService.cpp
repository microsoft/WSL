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
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;

bool ConsoleService::RelayInteractiveTty(ClientRunningWSLCProcess& Process, HANDLE Tty, bool triggerRefresh)
{
    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console;

    if (triggerRefresh)
    {
        // In the case of an Attach, force a terminal resize to force the tty to refresh its display.
        // The docker client uses the same trick.

        auto size = console.GetWindowSize();

        LOG_IF_FAILED(Process.Get().ResizeTty(size.Y + 1, size.X + 1));
        LOG_IF_FAILED(Process.Get().ResizeTty(size.Y, size.X));
    }

    wil::unique_event exitEvent(wil::EventOptions::ManualReset);

    bool completed = false;

    // Create a thread to relay stdin to the pipe.
    std::thread inputThread([&]() {
        auto updateTerminal = [&console, &Process]() {
            const auto windowSize = console.GetWindowSize();
            LOG_IF_FAILED(Process.Get().ResizeTty(windowSize.Y, windowSize.X));
        };

        // TODO: Make this configurable (default to ctrl-p, ctrl-q).
        std::vector<char> detachSequence{0x10, 0x11};

        completed = wsl::windows::common::relay::StandardInputRelay(
            GetStdHandle(STD_INPUT_HANDLE), Tty, updateTerminal, exitEvent.get(), detachSequence);
    });

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        exitEvent.SetEvent();
        inputThread.join();
    });

    // Relay the contents of the pipe to stdout.
    wsl::windows::common::relay::InterruptableRelay(Tty, GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());

    joinThread.reset();

    return completed;
}

void ConsoleService::RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr)
{
    wsl::windows::common::relay::MultiHandleWait io;

    // Create a thread to relay stdin to the pipe.
    wil::unique_event exitEvent(wil::EventOptions::ManualReset);

    std::thread inputThread;

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (inputThread.joinable())
        {
            exitEvent.SetEvent();
            inputThread.join();
        }
    });

    if (Stdin.is_valid())
    {
        // Required because ReadFile() blocks if stdin doesn't support overlapped IO.
        // This can create pipe deadlocks if we get blocked reading stdin while data is available on stdout / stderr.
        // TODO: Will output CR instead of LF's which can confuse the linux app.
        // Consider a custom relay logic to fix this.
        inputThread = std::thread{[&]() {
            try
            {
                wsl::windows::common::relay::InterruptableRelay(GetStdHandle(STD_INPUT_HANDLE), Stdin.get(), exitEvent.get());
            }
            CATCH_LOG();

            Stdin.reset();
        }};
    }

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stdout), GetStdHandle(STD_OUTPUT_HANDLE)));
    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stderr), GetStdHandle(STD_ERROR_HANDLE)));

    io.Run({});
}

int ConsoleService::AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLCProcess&& process)
{
    if (WI_IsFlagSet(process.Flags(), WSLCProcessFlagsTty))
    {
        if (!RelayInteractiveTty(process, process.GetStdHandle(WSLCFDTty).get()))
        {
            wsl::windows::common::wslutil::PrintMessage(L"[detached]", stderr);
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
