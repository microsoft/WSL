// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "securityaudit/ProcessCreationAudit.h"
#include "SecurityAudit.h"
#include "lxinitshared.h"
#include "stringshared.h"
#include "wslsecurityaudit.h"
#include <nlohmann/json.hpp>

namespace wsl::windows::common::securityaudit {

static_assert(MSG_PROCESS_CREATED <= UINT16_MAX);

std::wstring FormatProcessCreation(gsl::span<gsl::byte> bytes)
{
    using Json = nlohmann::json;
    const auto* event = gslhelpers::try_get_struct<LX_INIT_PROCESS_CREATED>(bytes);
    THROW_HR_IF(
        E_INVALIDARG,
        !event || bytes.size() != sizeof(*event) || event->Header.MessageSize != sizeof(*event) ||
            event->Header.MessageType != event->Type || event->ChildPid == 0 || event->ChildTid == 0);

    Json data{{"schemaVersion", 1}, {"childPid", event->ChildPid}, {"childTid", event->ChildTid}};
    if (event->ParentPid != 0)
    {
        data["parentPid"] = event->ParentPid;
    }
    if (event->Uid != UINT32_MAX)
    {
        data["uid"] = event->Uid;
    }
    if (event->Gid != UINT32_MAX)
    {
        data["gid"] = event->Gid;
    }
    for (const auto& [name, value] :
         {std::pair{"pid", event->PidNamespace}, {"mount", event->MountNamespace}, {"network", event->NetworkNamespace}})
    {
        if (value != 0)
        {
            data["namespaceIds"][name] = value;
        }
    }
    if (event->CgroupId != 0)
    {
        data["cgroupId"] = std::to_string(event->CgroupId);
    }
    if (event->TimestampNs != 0)
    {
        data["timestampNs"] = std::to_string(event->TimestampNs);
        data["timestampClock"] = "monotonic";
    }
    return wsl::shared::string::MultiByteToWide(data.dump());
}

std::wstring FormatProcessCreationDisplay(std::wstring_view details)
{
    using Json = nlohmann::json;
    const auto data = Json::parse(wsl::shared::string::WideToMultiByte(std::wstring(details)));
    THROW_HR_IF(E_INVALIDARG, !data.is_object());
    const auto namespaces = data.value("namespaceIds", Json::object());
    THROW_HR_IF(E_INVALIDARG, !namespaces.is_object());
    return NormalizeEventMessage(
        wsl::shared::Localization::MessageSecurityAuditProcessCreationDisplay(
            FormatEventField(data, "parentPid"),
            FormatEventField(data, "childPid"),
            FormatEventField(data, "childTid"),
            FormatEventField(data, "timestampNs"),
            FormatEventField(data, "uid"),
            FormatEventField(data, "gid"),
            FormatEventField(data, "cgroupId"),
            FormatEventField(namespaces, "pid"),
            FormatEventField(namespaces, "mount"),
            FormatEventField(namespaces, "network")));
}

void ReportProcessCreation(EventSource& source, uint32_t childPid, std::wstring_view details)
{
    THROW_HR_IF(E_INVALIDARG, childPid == 0);
    std::array<std::wstring, 4> fields{
        std::to_wstring(childPid),
        wsl::shared::Localization::MessageSecurityAuditProcessCreation(),
        std::wstring(details),
        FormatProcessCreationDisplay(details)};
    source.Report(MSG_PROCESS_CREATED, fields);
}

} // namespace wsl::windows::common::securityaudit
