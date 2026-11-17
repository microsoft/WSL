// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "securityaudit/NetworkAudit.h"
#include "SecurityAudit.h"
#include "stringshared.h"
#include "wslsecurityaudit.h"
#include <ws2tcpip.h>
#include <nlohmann/json.hpp>

namespace wsl::windows::common::securityaudit {
namespace {
    const char* NetworkSyscallName(LX_NETWORK_AUDIT_OPERATION operation)
    {
        switch (operation)
        {
        case LX_NETWORK_AUDIT_OPERATION::Socket:
            return "socket";
        case LX_NETWORK_AUDIT_OPERATION::Connect:
            return "connect";
        case LX_NETWORK_AUDIT_OPERATION::Bind:
            return "bind";
        case LX_NETWORK_AUDIT_OPERATION::Accept:
            return "accept";
        case LX_NETWORK_AUDIT_OPERATION::Accept4:
            return "accept4";
        default:
            THROW_HR(E_INVALIDARG);
        }
    }

    void AddNetworkEndpoint(nlohmann::json& data, const LX_AUDIT_ENDPOINT& endpoint, const char* ipField, const char* portField = nullptr)
    {
        THROW_HR_IF(E_INVALIDARG, endpoint.Family != 0 && endpoint.Family != 4 && endpoint.Family != 6);
        if (endpoint.Family != 0)
        {
            std::array<char, INET6_ADDRSTRLEN> address{};
            THROW_HR_IF(
                E_INVALIDARG, InetNtopA(endpoint.Family == 4 ? AF_INET : AF_INET6, endpoint.Address, address.data(), address.size()) == nullptr);
            data[ipField] = address.data();
        }
        if (portField != nullptr)
        {
            THROW_HR_IF(E_INVALIDARG, endpoint.Port > UINT16_MAX);
            if (endpoint.Port != 0)
            {
                data[portField] = endpoint.Port;
            }
        }
    }
} // namespace

static_assert(MSG_NETWORK_SOCKET <= UINT16_MAX && MSG_NETWORK_CONNECT <= UINT16_MAX && MSG_NETWORK_BIND <= UINT16_MAX);
static_assert(MSG_NETWORK_ACCEPT <= UINT16_MAX);

std::wstring FormatNetworkAudit(gsl::span<gsl::byte> bytes)
{
    using Json = nlohmann::json;
    const auto* event = gslhelpers::try_get_struct<LX_INIT_NETWORK_AUDIT>(bytes);
    THROW_HR_IF(
        E_INVALIDARG,
        !event || bytes.size() != sizeof(*event) || event->Header.MessageSize != sizeof(*event) ||
            event->Header.MessageType != event->Type || (event->Fields & ~(LxNetworkAuditResult | LxNetworkAuditProtocol)) != 0);

    Json data{{"schemaVersion", 1}, {"syscall", NetworkSyscallName(event->Operation)}};
    if (event->Pid != 0)
    {
        data["pid"] = event->Pid;
    }
    if (event->Uid != UINT32_MAX)
    {
        data["uid"] = event->Uid;
    }
    if (event->Euid != UINT32_MAX)
    {
        data["euid"] = event->Euid;
    }
    for (const auto& [name, value] :
         {std::pair{"pid", event->PidNamespace}, {"mount", event->MountNamespace}, {"network", event->NetworkNamespace}})
    {
        if (value != 0)
        {
            data["namespaceIds"][name] = value;
        }
    }
    if (event->Fields & LxNetworkAuditResult)
    {
        THROW_HR_IF(E_INVALIDARG, event->Result < -4095);
        data["result"] = event->Result;
        if (event->Result < 0)
        {
            data["error"] = -event->Result;
        }
    }
    switch (event->Operation)
    {
    case LX_NETWORK_AUDIT_OPERATION::Socket:
        if (event->Fields & LxNetworkAuditProtocol)
        {
            data["protocol"] = event->Protocol;
        }
        break;
    case LX_NETWORK_AUDIT_OPERATION::Bind:
        AddNetworkEndpoint(data, event->Local, "localIp");
        break;
    case LX_NETWORK_AUDIT_OPERATION::Connect:
    case LX_NETWORK_AUDIT_OPERATION::Accept:
    case LX_NETWORK_AUDIT_OPERATION::Accept4:
        AddNetworkEndpoint(data, event->Local, "localIp", "localPort");
        AddNetworkEndpoint(data, event->Remote, "remoteIp", "remotePort");
        break;
    default:
        break;
    }
    return wsl::shared::string::MultiByteToWide(EscapeEventParameters(data.dump()));
}

std::wstring FormatNetworkAuditDisplay(LX_NETWORK_AUDIT_OPERATION operation, std::wstring_view details)
{
    using Json = nlohmann::json;
    using wsl::shared::Localization;
    const auto data = Json::parse(wsl::shared::string::WideToMultiByte(std::wstring(details)));
    THROW_HR_IF(E_INVALIDARG, !data.is_object());
    const auto namespaces = data.value("namespaceIds", Json::object());
    THROW_HR_IF(E_INVALIDARG, !namespaces.is_object());
    if (operation == LX_NETWORK_AUDIT_OPERATION::Socket)
    {
        return FormatSyscallEventDisplay(
            NetworkSyscallName(operation),
            Localization::MessageSecurityAuditNetworkSocketDisplay(
                FormatEventField(data, "pid"),
                FormatEventField(data, "uid"),
                FormatEventField(data, "euid"),
                FormatEventField(data, "protocol"),
                FormatEventField(namespaces, "pid"),
                FormatEventField(namespaces, "mount"),
                FormatEventField(namespaces, "network"),
                FormatEventField(data, "result"),
                FormatEventField(data, "error")));
    }
    if (operation == LX_NETWORK_AUDIT_OPERATION::Bind)
    {
        return FormatSyscallEventDisplay(
            NetworkSyscallName(operation),
            Localization::MessageSecurityAuditNetworkBindDisplay(
                FormatEventField(data, "pid"),
                FormatEventField(data, "uid"),
                FormatEventField(data, "euid"),
                FormatEventField(namespaces, "pid"),
                FormatEventField(namespaces, "mount"),
                FormatEventField(namespaces, "network"),
                FormatEventField(data, "localIp"),
                FormatEventField(data, "result"),
                FormatEventField(data, "error")));
    }
    return FormatSyscallEventDisplay(
        NetworkSyscallName(operation),
        Localization::MessageSecurityAuditNetworkEndpointsDisplay(
            FormatEventField(data, "pid"),
            FormatEventField(data, "uid"),
            FormatEventField(data, "euid"),
            FormatEventField(namespaces, "pid"),
            FormatEventField(namespaces, "mount"),
            FormatEventField(namespaces, "network"),
            FormatEventField(data, "localIp"),
            FormatEventField(data, "localPort"),
            FormatEventField(data, "remoteIp"),
            FormatEventField(data, "remotePort"),
            FormatEventField(data, "result"),
            FormatEventField(data, "error")));
}

void ReportNetworkAudit(EventSource& source, const LX_INIT_NETWORK_AUDIT& event, std::wstring_view details)
{
    using wsl::shared::Localization;
    DWORD eventId{};
    std::wstring description;
    switch (event.Operation)
    {
    case LX_NETWORK_AUDIT_OPERATION::Socket:
        eventId = MSG_NETWORK_SOCKET;
        description = Localization::MessageSecurityAuditNetworkSocket();
        break;
    case LX_NETWORK_AUDIT_OPERATION::Connect:
        eventId = MSG_NETWORK_CONNECT;
        description = Localization::MessageSecurityAuditNetworkConnect();
        break;
    case LX_NETWORK_AUDIT_OPERATION::Bind:
        eventId = MSG_NETWORK_BIND;
        description = Localization::MessageSecurityAuditNetworkBind();
        break;
    case LX_NETWORK_AUDIT_OPERATION::Accept:
    case LX_NETWORK_AUDIT_OPERATION::Accept4:
        eventId = MSG_NETWORK_ACCEPT;
        description = Localization::MessageSecurityAuditNetworkAccept();
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }
    std::array<std::wstring, 4> fields{
        event.Pid != 0 ? std::to_wstring(event.Pid) : L"",
        std::move(description),
        std::wstring(details),
        FormatNetworkAuditDisplay(event.Operation, details)};
    source.Report(eventId, fields);
}

} // namespace wsl::windows::common::securityaudit
