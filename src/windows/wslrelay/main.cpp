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
    uint32_t port{};
    GUID vmId{};
    bool disableTelemetry = !wsl::shared::OfficialBuild;
    bool connectPipe = false;

    ArgumentParser parser(GetCommandLineW(), wslrelay::binary_name);
    parser.AddArgument(Integer(reinterpret_cast<int&>(mode)), wslrelay::mode_option);
    parser.AddArgument(Handle{handle}, wslrelay::handle_option);
    parser.AddArgument(vmId, wslrelay::vm_id_option);
    parser.AddArgument(Handle{pipe}, wslrelay::pipe_option);
    parser.AddArgument(Handle{exitEvent}, wslrelay::exit_event_option);
    parser.AddArgument(Integer{port}, wslrelay::port_option);
    parser.AddArgument(disableTelemetry, wslrelay::disable_telemetry_option);
    parser.AddArgument(connectPipe, wslrelay::connect_pipe_option);
    parser.Parse();

    // Initialize logging.
    WslTraceLoggingInitialize(LxssTelemetryProvider, disableTelemetry);
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { WslTraceLoggingUninitialize(); });

    // Ensure that the other end of the pipe has connected if required.
    if (connectPipe)
    {
        std::vector<HANDLE> exitEvents;
        if (exitEvent)
        {
            exitEvents.push_back(exitEvent.get());
        }

        wsl::windows::common::helpers::ConnectPipe(pipe.get(), (15 * 1000), exitEvents);
    }

    // Perform the requested operation.
    switch (mode)
    {
    case wslrelay::RelayMode::DebugConsole:
    {
        // If not relaying to a file, create a console window.
        if (!handle)
        {
            wsl::windows::common::helpers::CreateConsole(L"WSL Debug Console");
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

    case wslrelay::RelayMode::KdRelay:
    {
        THROW_HR_IF(E_INVALIDARG, port == 0);

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

    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid relay mode %d specified.", static_cast<int>(mode));
    }

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 1;
}
