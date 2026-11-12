// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "securityaudit/ProcessExecutionAudit.h"
#include "SecurityAudit.h"
#include "lxinitshared.h"
#include "stringshared.h"
#include "wslsecurityaudit.h"
#include <wil/token_helpers.h>
#include <nlohmann/json.hpp>

namespace wsl::windows::common::securityaudit {

static_assert(MSG_PROCESS_EXECUTED_DETAILS <= UINT16_MAX);

bool ProcessExecutionEnabled()
{
    return Enabled();
}

bool IncludeProcessCommandLine()
{
    auto self = wil::run_as_self();
    const auto value = registry::ReadDword(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\Audit",
        L"ProcessCreationIncludeCmdLine_Enabled",
        0);
    THROW_HR_IF(E_INVALIDARG, value > 1);
    return value == 1;
}

std::wstring FormatProcessExecution(gsl::span<gsl::byte> bytes, bool includeCommandLine, const ProcessExecutionDistribution* distribution)
{
    using Json = nlohmann::json;
    const auto* event = gslhelpers::try_get_struct<LX_INIT_PROCESS_EXECUTED_DETAILS>(bytes);
    THROW_HR_IF(
        E_INVALIDARG,
        !event || bytes.size() > 32 * 1024 || event->Header.MessageSize != bytes.size() ||
            event->Header.MessageType != event->Type || event->Pid == 0 || (event->Flags & ~7U) != 0);
    Json data{{"schemaVersion", 1}, {"pid", event->Pid}};
    if (distribution != nullptr)
    {
        data["distroId"] = wsl::shared::string::GuidToString<char>(distribution->Id);
        data["distro"] = wsl::shared::string::WideToMultiByte(distribution->Name);
    }
    const auto number = [&](const char* name, uint32_t value, uint32_t unavailable = 0) {
        if (value != unavailable)
        {
            data[name] = value;
        }
    };
    number("tid", event->Tid);
    number("ppid", event->Ppid);
    number("uid", event->Uid, UINT32_MAX);
    number("euid", event->Euid, UINT32_MAX);
    number("gid", event->Gid, UINT32_MAX);
    number("egid", event->Egid, UINT32_MAX);
    for (const auto& [name, value] :
         {std::pair{"pid", event->PidNamespace}, {"mount", event->MountNamespace}, {"network", event->NetworkNamespace}})
    {
        if (value != 0)
        {
            data["namespaceIds"][name] = value;
        }
    }
    if (event->TimestampNs != 0)
    {
        data["timestampNs"] = std::to_string(event->TimestampNs);
        data["timestampClock"] = "monotonic";
    }
    const auto addText = [&](const char* name, Json value) {
        try
        {
            (void)value.dump(); // Validate Linux byte strings before passing them to a Unicode log.
            data[name] = std::move(value);
        }
        catch (const Json::type_error&)
        {
            LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION), "Omitted non-UTF-8 audit field: %hs", name);
            data["invalidFields"].push_back(name);
        }
    };
    const auto text = [&](const char* name, uint32_t offset, size_t maximum) {
        if (offset != 0)
        {
            THROW_HR_IF(E_INVALIDARG, offset < sizeof(*event));
            const std::string value = wsl::shared::string::FromSpan(bytes, offset);
            THROW_HR_IF(E_INVALIDARG, value.size() > maximum);
            addText(name, value);
        }
    };
    text("executablePath", event->ExecutablePathOffset, 4096);
    text("executable", event->ExecutableOffset, 15);
    text("cwd", event->CwdOffset, 4096);
    if (includeCommandLine && event->ArgvOffset != 0)
    {
        THROW_HR_IF(
            E_INVALIDARG,
            event->ArgvOffset < sizeof(*event) || event->ArgvOffset > bytes.size() || event->ArgvBytes > 4097 ||
                event->ArgvBytes == 0 || event->ArgvBytes > bytes.size() - event->ArgvOffset);
        auto remaining = bytes.subspan(event->ArgvOffset, event->ArgvBytes);
        Json arguments = Json::array();
        while (!remaining.empty())
        {
            THROW_HR_IF(E_INVALIDARG, arguments.size() >= 20);
            const auto* argument = wsl::shared::string::FromSpan(remaining);
            const auto length = strlen(argument);
            arguments.push_back(std::string(argument, length));
            remaining = remaining.subspan(length + 1);
        }
        addText("argv", std::move(arguments));
    }
    for (const auto& [name, flag] :
         {std::pair{"executablePath", LxProcessExecutablePathTruncated}, {"cwd", LxProcessCwdTruncated}, {"argv", LxProcessArgvTruncated}})
    {
        if (data.contains(name) && (event->Flags & flag) != 0)
        {
            data["truncatedFields"].push_back(name);
        }
    }
    const auto serialize = [&] {
        const auto json = data.dump(-1, ' ', true);
        std::string escaped;
        for (const char character : json)
        {
            if (character == '%')
            {
                escaped += "\\u0025"; // Prevent Event Viewer parameter-message expansion.
            }
            else
            {
                escaped += character;
            }
        }
        return escaped;
    };
    auto json = serialize();
    // Keep a conservative insertion-string budget without losing the entire observation.
    constexpr size_t c_maxLogCharacters = 12 * 1024;
    for (const auto* name : {"argv", "executablePath", "cwd"})
    {
        if (json.size() <= c_maxLogCharacters)
        {
            break;
        }
        if (data.erase(name) != 0)
        {
            data["omittedFields"].push_back(name);
            LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW), "Omitted oversized audit field: %hs", name);
            json = serialize();
        }
    }
    THROW_HR_IF(E_INVALIDARG, json.size() > c_maxLogCharacters);
    return wsl::shared::string::MultiByteToWide(json);
}

void ReportProcessExecution(EventSource& source, uint32_t pid)
{
    THROW_HR_IF(E_INVALIDARG, pid == 0);
    std::array<std::wstring, 3> fields{
        std::to_wstring(pid),
        wsl::shared::Localization::MessageSecurityAuditProcessExecuted(),
        wsl::shared::Localization::MessageSecurityAuditProcessId()};
    source.Report(MSG_PROCESS_EXECUTED, fields);
}

void ReportProcessExecutionDetails(EventSource& source, uint32_t pid, std::wstring_view details)
{
    THROW_HR_IF(E_INVALIDARG, pid == 0);
    std::array<std::wstring, 3> fields{
        std::to_wstring(pid), wsl::shared::Localization::MessageSecurityAuditProcessExecution(), std::wstring(details)};
    source.Report(MSG_PROCESS_EXECUTED_DETAILS, fields);
}

} // namespace wsl::windows::common::securityaudit
