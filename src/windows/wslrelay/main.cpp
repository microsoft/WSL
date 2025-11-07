/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entrypoint for wslrelay.

--*/

#include "precomp.h"
#include "localhost.h"
#include "CommandLine.h"

using namespace wsl::windows::common;
using namespace wsl::shared;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
try
{
    wsl::windows::common::wslutil::ConfigureCrt();
    wsl::windows::common::wslutil::InitializeWil();

    // Initialize COM.
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wsl::windows::common::wslutil::CoInitializeSecurity();

    // Initialize winsock.
    WSADATA data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));

    // Parse arguments.
    wil::unique_handle handle{};
    wslrelay::RelayMode mode{wslrelay::RelayMode::Invalid};
    wil::unique_handle pipe{};
    wil::unique_handle exitEvent{};
    wil::unique_handle terminalInputHandle{};
    wil::unique_handle terminalOutputHandle{};
    wil::unique_socket terminalControlHandle{};
    uint32_t port{};
    GUID vmId{};
    bool disableTelemetry = !wsl::shared::OfficialBuild;

    ArgumentParser parser(GetCommandLineW(), wslrelay::binary_name);
    parser.AddArgument(Integer(reinterpret_cast<int&>(mode)), wslrelay::mode_option);
    parser.AddArgument(Handle{handle}, wslrelay::handle_option);
    parser.AddArgument(vmId, wslrelay::vm_id_option);
    parser.AddArgument(Handle{pipe}, wslrelay::pipe_option);
    parser.AddArgument(Handle{exitEvent}, wslrelay::exit_event_option);
    parser.AddArgument(Integer{port}, wslrelay::port_option);
    parser.AddArgument(disableTelemetry, wslrelay::disable_telemetry_option);
    parser.AddArgument(Handle{terminalInputHandle}, wslrelay::input_option);
    parser.AddArgument(Handle{terminalOutputHandle}, wslrelay::output_option);
    parser.AddArgument(Handle<wil::unique_socket>{terminalControlHandle}, wslrelay::control_option);
    parser.Parse();

    // Initialize logging.
    WslTraceLoggingInitialize(LxssTelemetryProvider, disableTelemetry);
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { WslTraceLoggingUninitialize(); });

    // Perform the requested operation.
    switch (mode)
    {
    case wslrelay::RelayMode::DebugConsole:
    case wslrelay::RelayMode::DebugConsoleRelay:
    {
        // If not relaying to a file, create a console window.
        if (!handle)
        {
            wsl::windows::common::helpers::CreateConsole(L"WSL Debug Console");
        }

        if (mode == wslrelay::RelayMode::DebugConsole)
        {
            // Ensure that the other end of the pipe has connected.
            wsl::windows::common::helpers::ConnectPipe(pipe.get(), (15 * 1000));
        }

        // Relay the contents of the pipe to the output handle.
        wsl::windows::common::relay::InterruptableRelay(pipe.get(), handle ? handle.get() : GetStdHandle(STD_OUTPUT_HANDLE));

        // Print a message that the VM has exited and prompt the user for input.
        wsl::windows::common::wslutil::PrintSystemError(HCS_E_CONNECTION_CLOSED);
        if (!handle)
        {
            getwchar();
        }

        break;
    }

    case wslrelay::RelayMode::PortRelay:
    {
        wsl::shared::SocketChannel channel{wil::unique_socket{reinterpret_cast<SOCKET>(handle.release())}, "PortRelay"};
        wsl::windows::wslrelay::localhost::RelayWorker(channel, vmId);
        break;
    }

    case wslrelay::RelayMode::WSLAPortRelay:
    {
        try
        {
            wsl::windows::wslrelay::localhost::RunWSLAPortRelay(vmId, port, exitEvent.get());
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
        }

        break;
    }

    case wslrelay::RelayMode::KdRelay:
    {
        THROW_HR_IF(E_INVALIDARG, port == 0);

        // Ensure that the other end of the pipe has connected.
        wsl::windows::common::helpers::ConnectPipe(pipe.get(), (15 * 1000), {exitEvent.get()});

        // Bind, listen, and accept a connection on the specified port.
        const wil::unique_socket listenSocket(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));
        THROW_LAST_ERROR_IF(!listenSocket);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        THROW_LAST_ERROR_IF(bind(listenSocket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

        THROW_LAST_ERROR_IF(listen(listenSocket.get(), 1) == SOCKET_ERROR);

        const wil::unique_socket socket(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));
        THROW_LAST_ERROR_IF(!socket);

        wsl::windows::common::socket::Accept(listenSocket.get(), socket.get(), INFINITE, exitEvent.get());

        // Begin the relay.
        wsl::windows::common::relay::BidirectionalRelay(
            reinterpret_cast<HANDLE>(socket.get()), pipe.get(), 0x1000, wsl::windows::common::relay::RelayFlags::LeftIsSocket);

        break;
    }

    case wslrelay::RelayMode::InteractiveConsoleRelay:
    {
        THROW_HR_IF(E_INVALIDARG, !terminalInputHandle || !terminalOutputHandle);

        AllocConsole();
        auto consoleOutputHandle = wil::unique_handle{CreateFileW(
            L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};

        THROW_LAST_ERROR_IF(!consoleOutputHandle.is_valid());

        auto consoleInputHandle = wil::unique_handle{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};

        THROW_LAST_ERROR_IF(!consoleInputHandle.is_valid());

        // Configure console for interactive usage.

        {
            DWORD OutputMode{};
            THROW_LAST_ERROR_IF(!::GetConsoleMode(consoleOutputHandle.get(), &OutputMode));

            WI_SetAllFlags(OutputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
            THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleOutputHandle.get(), OutputMode));
        }

        {
            DWORD InputMode{};
            THROW_LAST_ERROR_IF(!::GetConsoleMode(consoleInputHandle.get(), &InputMode));

            WI_SetAllFlags(InputMode, (ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
            WI_ClearAllFlags(InputMode, (ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
            THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleInputHandle.get(), InputMode));
        }

        THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

        // Create a thread to relay stdin to the pipe.
        auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);

        std::optional<wsl::shared::SocketChannel> controlChannel;
        if (terminalControlHandle)
        {
            controlChannel.emplace(std::move(terminalControlHandle), "TerminalControl", exitEvent.get());
        }

        std::thread inputThread([&]() {
            auto updateTerminal = [&controlChannel, &consoleOutputHandle]() {
                if (controlChannel.has_value())
                {
                    CONSOLE_SCREEN_BUFFER_INFOEX info{};
                    info.cbSize = sizeof(info);

                    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(consoleOutputHandle.get(), &info));

                    WSLA_TERMINAL_CHANGED message{};
                    message.Columns = info.srWindow.Right - info.srWindow.Left + 1;
                    message.Rows = info.srWindow.Bottom - info.srWindow.Top + 1;

                    controlChannel->SendMessage(message);
                }
            };

            wsl::windows::common::relay::StandardInputRelay(
                GetStdHandle(STD_INPUT_HANDLE), terminalInputHandle.get(), updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the contents of the pipe to stdout.
        wsl::windows::common::relay::InterruptableRelay(terminalOutputHandle.get(), GetStdHandle(STD_OUTPUT_HANDLE));

        // TODO: watch process exit code.

        break;
    }

    default:
        THROW_HR(E_INVALIDARG);
    }

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 1;
}
