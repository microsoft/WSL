#include <precomp.h>
#include <WSLAProcessLauncher.h>
#include "ConsoleService.h"

namespace wslc::services
{

std::vector<WSLA_PROCESS_FD> ConsoleService::BuildStdioDescriptors(bool tty, bool interactive)
{
    std::vector<WSLA_PROCESS_FD> fds;
    if (tty)
    {
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});
    }
    else
    {
        if (interactive)
        {
            fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault});
    }

    return fds;
}

int ConsoleService::AttachToCurrentConsole(wil::com_ptr<IWSLAProcess>&& wslaProcess, wslc::models::ConsoleAttachOptions options)
{
    auto fds = BuildStdioDescriptors(options.TTY, options.Interactive);
    auto process = wsl::windows::common::ClientRunningWSLAProcess(std::move(wslaProcess), std::move(fds));
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);
    auto exitEvent = process.GetExitEvent();

    if (options.TTY)
    {
        // Save original console modes so they can be restored on exit.
        DWORD OriginalInputMode{};
        DWORD OriginalOutputMode{};
        UINT OriginalOutputCP = GetConsoleOutputCP();
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdin, &OriginalInputMode));
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdout, &OriginalOutputMode));

        auto restoreConsoleMode = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            SetConsoleMode(Stdin, OriginalInputMode);
            SetConsoleMode(Stdout, OriginalOutputMode);
            SetConsoleOutputCP(OriginalOutputCP);
        });

        // Configure console for interactive usage.
        DWORD InputMode = OriginalInputMode;
        WI_SetAllFlags(InputMode, (ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
        WI_ClearAllFlags(InputMode, (ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdin, InputMode));

        DWORD OutputMode = OriginalOutputMode;
        WI_SetAllFlags(OutputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdout, OutputMode));

        THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

        auto processTty = process.GetStdHandle(WSLAFDTty);

        // TODO: Study a single thread for both handles.

        // Create a thread to relay stdin to the pipe.
        std::thread inputThread([&]() {
            auto updateTerminal = [&Stdout, &process, &processTty]() {
                CONSOLE_SCREEN_BUFFER_INFOEX info{};
                info.cbSize = sizeof(info);

                THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(Stdout, &info));

                LOG_IF_FAILED(process.Get().ResizeTty(
                    info.srWindow.Bottom - info.srWindow.Top + 1, info.srWindow.Right - info.srWindow.Left + 1));
            };

            wsl::windows::common::relay::StandardInputRelay(Stdin, processTty.get(), updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the contents of the pipe to stdout.
        wsl::windows::common::relay::InterruptableRelay(processTty.get(), Stdout);

        // Wait for the process to exit.
        THROW_LAST_ERROR_IF(WaitForSingleObject(exitEvent.get(), INFINITE) != WAIT_OBJECT_0);
    }
    else
    {
        wsl::windows::common::relay::MultiHandleWait io;

        // Create a thread to relay stdin to the pipe.

        std::thread inputThread;

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (inputThread.joinable())
            {
                exitEvent.SetEvent();
                inputThread.join();
            }
        });

        // Required because ReadFile() blocks if stdin is a tty.
        if (wsl::windows::common::wslutil::IsInteractiveConsole())
        {
            // TODO: Will output CR instead of LF's which can confuse the linux app.
            // Consider a custom relay logic to fix this.
            inputThread = std::thread{
                [&]() { wsl::windows::common::relay::InterruptableRelay(Stdin, process.GetStdHandle(0).get(), exitEvent.get()); }};
        }
        else
        {
            io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle>(GetStdHandle(STD_INPUT_HANDLE), process.GetStdHandle(0)));
        }

        io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle>(process.GetStdHandle(1), GetStdHandle(STD_OUTPUT_HANDLE)));
        io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle>(process.GetStdHandle(2), GetStdHandle(STD_ERROR_HANDLE)));
        io.AddHandle(std::make_unique<wsl::windows::common::relay::EventHandle>(exitEvent.get()));

        io.Run({});
    }

    return process.GetExitCode();
}
}