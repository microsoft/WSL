/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleService.cpp

Abstract:

    This file contains the ConsoleService implementation

--*/
#include <precomp.h>
#include <WSLAProcessLauncher.h>
#include "ConsoleService.h"

namespace wsl::windows::wslc::services {

using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;

static void RelayInteractiveTty(ClientRunningWSLAProcess& Process, HANDLE Tty, bool triggerRefresh = false)
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

    // Create a thread to relay stdin to the pipe.
    std::thread inputThread([&]() {
        auto updateTerminal = [&console, &Process]() {
            const auto windowSize = console.GetWindowSize();
            LOG_IF_FAILED(Process.Get().ResizeTty(windowSize.Y, windowSize.X));
        };

        wsl::windows::common::relay::StandardInputRelay(GetStdHandle(STD_INPUT_HANDLE), Tty, updateTerminal, exitEvent.get());
    });

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        exitEvent.SetEvent();
        inputThread.join();
    });

    // Relay the contents of the pipe to stdout.
    wsl::windows::common::relay::InterruptableRelay(Tty, GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());
}

static void RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr)
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
        // Required because ReadFile() blocks if stdin is a tty.
        if (wsl::windows::common::wslutil::IsInteractiveConsole())
        {
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
        else
        {
            io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(GetStdHandle(STD_INPUT_HANDLE), std::move(Stdin)));
        }
    }

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stdout), GetStdHandle(STD_OUTPUT_HANDLE)));
    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stderr), GetStdHandle(STD_ERROR_HANDLE)));

    io.Run({});
}

int ConsoleService::AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLAProcess&& process)
{
    if (WI_IsFlagSet(process.Flags(), WSLAProcessFlagsTty))
    {
        RelayInteractiveTty(process, process.GetStdHandle(WSLAFDTty).get());
    }
    else
    {
        wil::unique_handle stdinHandle;
        if (WI_IsFlagSet(process.Flags(), WSLAProcessFlagsStdin))
        {
            stdinHandle = process.GetStdHandle(WSLAFDStdin);
        }

        RelayNonTtyProcess(std::move(stdinHandle), process.GetStdHandle(WSLAFDStdout), process.GetStdHandle(WSLAFDStderr));
    }

    return process.Wait();
}
} // namespace wsl::windows::wslc::services
