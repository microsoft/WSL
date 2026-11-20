// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "GuestConnector.h"
#include "hvsocket.hpp"
#include "vsock.hpp"

namespace {

constexpr std::wstring_view c_hcsPrefix = L"hcs:";
constexpr std::wstring_view c_openVmmPrefix = L"openvmm:";

} // namespace

wsl::windows::common::GuestConnector::GuestConnector(HcsConnection Connection) : m_connection(std::move(Connection))
{
}

wsl::windows::common::GuestConnector::GuestConnector(OpenVmmConnection Connection) : m_connection(std::move(Connection))
{
}

wsl::windows::common::GuestConnector wsl::windows::common::GuestConnector::CreateHcs(_In_ const GUID& VmId)
{
    return GuestConnector{HcsConnection{VmId}};
}

wsl::windows::common::GuestConnector wsl::windows::common::GuestConnector::CreateOpenVmm(_In_ std::filesystem::path VsockPath)
{
    THROW_HR_IF(E_INVALIDARG, VsockPath.empty());
    return GuestConnector{OpenVmmConnection{std::move(VsockPath)}};
}

wsl::windows::common::GuestConnector wsl::windows::common::GuestConnector::Deserialize(_In_ std::wstring_view Value)
{
    if (Value.starts_with(c_hcsPrefix))
    {
        GUID vmId{};
        const std::wstring serializedGuid{Value.substr(c_hcsPrefix.size())};
        THROW_IF_FAILED(CLSIDFromString(serializedGuid.c_str(), &vmId));
        return CreateHcs(vmId);
    }

    if (Value.starts_with(c_openVmmPrefix))
    {
        return CreateOpenVmm(std::filesystem::path{std::wstring{Value.substr(c_openVmmPrefix.size())}});
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid guest connection handle");
}

wil::unique_socket wsl::windows::common::GuestConnector::Connect(_In_ unsigned long Port, _In_opt_ HANDLE ExitHandle) const
{
    return std::visit(
        [&](const auto& connection) -> wil::unique_socket {
            using Connection = std::decay_t<decltype(connection)>;
            if constexpr (std::is_same_v<Connection, HcsConnection>)
            {
                return hvsocket::Connect(connection.VmId, Port, ExitHandle);
            }
            else if constexpr (std::is_same_v<Connection, OpenVmmConnection>)
            {
                return vsock::Connect(connection.VsockPath, Port, ExitHandle);
            }
            else
            {
                THROW_HR_MSG(E_UNEXPECTED, "Guest connection handle is not initialized");
            }
        },
        m_connection);
}

std::wstring wsl::windows::common::GuestConnector::Serialize() const
{
    return std::visit(
        [](const auto& connection) -> std::wstring {
            using Connection = std::decay_t<decltype(connection)>;
            if constexpr (std::is_same_v<Connection, HcsConnection>)
            {
                return std::wstring{c_hcsPrefix} + wsl::shared::string::GuidToString<wchar_t>(connection.VmId);
            }
            else if constexpr (std::is_same_v<Connection, OpenVmmConnection>)
            {
                return std::wstring{c_openVmmPrefix} + connection.VsockPath.native();
            }
            else
            {
                return {};
            }
        },
        m_connection);
}
