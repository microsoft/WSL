// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "NativeAuditLaunch.h"
#include "hvsocket.hpp"
#include "../service/exe/LxssCreateProcess.h"
#include "../../shared/securityaudit/Launch.h"

using wsl::windows::common::NativeAuditLaunch;

namespace {
constexpr DWORD c_launchTimeoutMs = 2000;

void CheckCancelled(HANDLE event)
{
    const auto result = WaitForSingleObject(event, 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), result != WAIT_TIMEOUT);
}

wil::unique_socket Connect(const GUID& runtimeId, ULONG port, HANDLE terminatingEvent, DWORD timeout)
{
    CheckCancelled(terminatingEvent);
    auto socket = wsl::windows::common::hvsocket::Create();
    u_long nonblocking = 1;
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ioctlsocket(socket.get(), FIONBIO, &nonblocking) == SOCKET_ERROR);
    SOCKADDR_HV address{};
    address.Family = AF_HYPERV;
    address.VmId = HV_GUID_WILDCARD;
    address.ServiceId = HV_GUID_WILDCARD;
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0);
    address.VmId = runtimeId;
    address.ServiceId = HV_GUID_VSOCK_TEMPLATE;
    address.ServiceId.Data1 = port;
    const auto result = connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result != 0)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), WSAGetLastError() != WSAEWOULDBLOCK);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        for (;;)
        {
            CheckCancelled(terminatingEvent);
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::chrono::steady_clock::now() >= deadline);
            fd_set writable{}, errors{};
            FD_ZERO(&writable);
            FD_ZERO(&errors);
            FD_SET(socket.get(), &writable);
            FD_SET(socket.get(), &errors);
            timeval interval{0, 50000};
            const auto ready = select(0, nullptr, &writable, &errors, &interval);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
            if (ready != 0)
            {
                int error{};
                int length = sizeof(error);
                THROW_HR_IF(
                    HRESULT_FROM_WIN32(WSAGetLastError()),
                    getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == SOCKET_ERROR);
                THROW_HR_IF(HRESULT_FROM_WIN32(error), error != 0);
                break;
            }
        }
    }
    CheckCancelled(terminatingEvent);
    nonblocking = 0;
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ioctlsocket(socket.get(), FIONBIO, &nonblocking) == SOCKET_ERROR);
    return socket;
}
} // namespace

NativeAuditLaunch::NativeAuditLaunch(const GUID& runtimeId, HANDLE terminatingEvent) : m_runtimeId(runtimeId)
{
    HANDLE duplicate{};
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(), terminatingEvent, GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0));
    m_terminatingEvent.reset(duplicate);
}

wil::unique_socket NativeAuditLaunch::CreateRootNamespaceProcess(const std::vector<std::string>& arguments, const Connector& connector) const
{
    THROW_HR_IF(E_INVALIDARG, arguments.empty() || arguments.front() != "/wsl-audit-collector");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_launchTimeoutMs);
    const auto remaining = [&]() {
        CheckCancelled(m_terminatingEvent.get());
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), duration.count() <= 0);
        return static_cast<DWORD>(duration.count());
    };
    const auto connect = [&](const GUID& runtimeId, ULONG port, HANDLE terminatingEvent) {
        return connector ? connector(runtimeId, port, terminatingEvent, remaining())
                         : Connect(runtimeId, port, terminatingEvent, remaining());
    };
    auto control = connect(m_runtimeId, wsl::audit::c_nativeAuditLaunchPort, m_terminatingEvent.get());
    wsl::shared::SocketChannel channel{std::move(control), "native-audit-launch", {m_terminatingEvent.get()}};
    std::vector<LPCSTR> argv;
    for (const auto& argument : arguments)
    {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    auto process = LxssCreateProcess::CreateLinuxProcess(
        argv.front(), argv.data(), m_runtimeId, channel, m_terminatingEvent.get(), remaining(), connect);
    CheckCancelled(m_terminatingEvent.get());
    return process;
}
