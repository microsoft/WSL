// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "vsock.hpp"
#include "socket.hpp"

namespace {

constexpr DWORD c_handshakeTimeoutMs = 30000;

} // namespace

SOCKADDR_UN wsl::windows::common::vsock::GetUnixSocketAddress(_In_ const std::filesystem::path& Path)
{
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = Path.string();
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';
    return address;
}

wil::unique_socket wsl::windows::common::vsock::Connect(_In_ const std::filesystem::path& VsockPath, _In_ unsigned long Port, _In_opt_ HANDLE ExitHandle)
{
    wil::unique_socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    const auto address = GetUnixSocketAddress(VsockPath);

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()), connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    const auto request = std::format("CONNECT {}\n", Port);
    wsl::windows::common::socket::Send(socket.get(), gsl::make_span(reinterpret_cast<const gsl::byte*>(request.data()), request.size()), ExitHandle);

    std::array<char, 64> response{};
    size_t responseLength = 0;
    for (; responseLength < response.size() - 1; ++responseLength)
    {
        const auto bytesRead = wsl::windows::common::socket::Receive(
            socket.get(), gsl::make_span(reinterpret_cast<gsl::byte*>(&response[responseLength]), 1), ExitHandle, MSG_WAITALL, c_handshakeTimeoutMs);
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), bytesRead == 0, "vsock bridge closed during CONNECT handshake");
        if (response[responseLength] == '\n')
        {
            ++responseLength;
            break;
        }
    }

    THROW_HR_IF_MSG(
        E_FAIL, responseLength == response.size() - 1 && response[responseLength - 1] != '\n', "vsock bridge response too long");
    const std::string_view responseView{response.data(), responseLength};
    THROW_HR_IF_MSG(E_FAIL, !responseView.starts_with("OK "), "vsock bridge CONNECT failed: %hs", response.data());
    return socket;
}
