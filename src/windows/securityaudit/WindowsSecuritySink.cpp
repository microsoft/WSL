// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WindowsSecuritySink.h"
#include <filesystem>
#include <wil/token_helpers.h>
#include "NativeAudit.h"
#include "SecuritySource.h"

#include "WindowsSecurityMessages.h"

namespace wsl::audit {
namespace security_detail {
    constexpr ULONG PolicySetFlags(ULONG flags)
    {
        return flags == 0 ? POLICY_AUDIT_EVENT_NONE : flags;
    }
} // namespace security_detail

namespace {
    using Json = nlohmann::json;
    using UniquePolicy = wil::unique_any<PAUDIT_POLICY_INFORMATION, decltype(&::AuditFree), ::AuditFree>;

    static_assert(WSL_SECURITY_PROCESS == c_securityProcessEventId);
    static_assert(WSL_PROCESS_EXECUTED == c_processExecutedEventId);
    static_assert(WSL_PROCESS_CREATED == c_processCreatedEventId);
    static_assert(c_observationFieldCount <= c_maxSecurityParameters);
    static_assert(c_processExecutedFieldCount == 6 && c_processCreatedFieldCount == 6 && c_observationFieldCount == 6);
    static_assert(static_cast<size_t>(SecurityProcessField::PolicySource) + 1 == c_securityFieldCount);
    static_assert(security_detail::PolicySetFlags(0) == POLICY_AUDIT_EVENT_NONE);
    static_assert(security_detail::PolicySetFlags(2) == POLICY_AUDIT_EVENT_FAILURE);
    static_assert(security_detail::PolicySetFlags(3) == (POLICY_AUDIT_EVENT_SUCCESS | POLICY_AUDIT_EVENT_FAILURE));

    std::wstring Wide(std::string_view value)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, value.find('\0') != std::string_view::npos, "Security fields cannot contain NUL.");
        if (value.empty())
        {
            return {};
        }
        THROW_HR_IF(E_INVALIDARG, value.size() > INT_MAX);
        const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        THROW_LAST_ERROR_IF_MSG(size == 0, "Security fields must contain valid UTF-8.");
        std::wstring result(static_cast<size_t>(size), L'\0');
        THROW_LAST_ERROR_IF(
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == 0);
        return result;
    }

    std::string Text(const Json& message, const char* name, size_t maximum)
    {
        const auto iterator = message.find(name);
        THROW_HR_IF_MSG(
            E_INVALIDARG, iterator == message.end() || !iterator->is_string(), "Missing or invalid Windows Security field: %hs.", name);
        const auto& text = iterator->get_ref<const std::string&>();
        THROW_HR_IF_MSG(E_INVALIDARG, text.size() > maximum, "Windows Security field exceeds its limit: %hs.", name);
        Wide(text);
        return text;
    }

    std::wstring Number(const Json& message, const char* name, uint64_t maximum)
    {
        const auto iterator = message.find(name);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            iterator == message.end() || !iterator->is_number_unsigned() || iterator->get<uint64_t>() > maximum,
            "Missing or invalid unsigned Windows Security field: %hs.",
            name);
        return std::to_wstring(iterator->get<uint64_t>());
    }

    std::wstring OptionalSession(const Json& message, const char* name)
    {
        const auto iterator = message.find(name);
        return iterator == message.end() || iterator->is_null() ? L"unknown" : Number(message, name, UINT32_MAX);
    }

    std::wstring Boolean(const Json& message, const char* name, std::optional<bool> fallback = {})
    {
        const auto iterator = message.find(name);
        if (iterator == message.end() && fallback)
        {
            return *fallback ? L"true" : L"false";
        }
        THROW_HR_IF_MSG(E_INVALIDARG, iterator == message.end() || !iterator->is_boolean(), "Invalid Security flag: %hs.", name);
        return iterator->get<bool>() ? L"true" : L"false";
    }

    bool IsGuid(std::string_view text)
    {
        if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        {
            text = text.substr(1, 36);
        }
        if (text.size() != 36)
        {
            return false;
        }
        for (size_t index = 0; index < text.size(); ++index)
        {
            const auto character = text[index];
            if (index == 8 || index == 13 || index == 18 || index == 23)
            {
                if (character != '-')
                {
                    return false;
                }
            }
            else if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F')))
            {
                return false;
            }
        }
        return true;
    }

    void ValidateUtc(std::string_view text)
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            text.size() < 20 || text.size() > 30 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
                text[16] != ':' || text.back() != 'Z',
            "receivedUtc must be a Windows-generated UTC ISO-8601 timestamp.");
        for (size_t index = 0; index < text.size() - 1; ++index)
        {
            if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || (index == 19 && text.size() > 20))
            {
                continue;
            }
            THROW_HR_IF(E_INVALIDARG, text[index] < '0' || text[index] > '9');
        }
        THROW_HR_IF(E_INVALIDARG, text.size() > 20 && (text[19] != '.' || text.size() == 21));
        const auto number = [&](size_t offset, size_t count) {
            WORD value{};
            for (size_t index = offset; index < offset + count; ++index)
            {
                value = static_cast<WORD>(value * 10 + text[index] - '0');
            }
            return value;
        };
        SYSTEMTIME time{};
        time.wYear = number(0, 4);
        time.wMonth = number(5, 2);
        time.wDay = number(8, 2);
        time.wHour = number(11, 2);
        time.wMinute = number(14, 2);
        time.wSecond = number(17, 2);
        FILETIME fileTime{};
        THROW_HR_IF_MSG(E_INVALIDARG, !SystemTimeToFileTime(&time, &fileTime), "Invalid receivedUtc calendar date.");
    }

    std::wstring ReadableObservation(const Json& field)
    {
        if (field.at("status") == "disabled_by_policy")
        {
            return L"Not recorded (disabled by policy)";
        }
        if (field.at("status") == "unavailable")
        {
            return L"Not available";
        }
        const auto& value = field.at("value");
        if (value.is_array())
        {
            if (value.empty())
            {
                return L"None (0 arguments)";
            }
            for (const auto& item : value)
            {
                Wide(item.get_ref<const std::string&>());
            }
            const auto encoded = value.dump();
            return Wide(std::string_view(encoded).substr(1, encoded.size() - 2));
        }
        if (value.is_string())
        {
            Wide(value.get_ref<const std::string&>());
        }
        return field.contains("clock") ? Wide(value.get_ref<const std::string&>()) : Wide(value.dump());
    }

    std::wstring EscapeObservationPercents(std::wstring text)
    {
        // Event Log expands percent sequences inside insertions again; JSON escaping keeps the original text recoverable.
        size_t offset{};
        while ((offset = text.find(L'%', offset)) != std::wstring::npos)
        {
            text.replace(offset, 1, L"\\u0025");
            offset += 6;
        }
        return text;
    }

    void ValidateProcessPayload(const SecurityEventPayload& payload)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, payload.fields.size() != 6, "PM Security audits require exactly six string insertions.");
        size_t characters{};
        for (const auto& field : payload.fields)
        {
            THROW_HR_IF_MSG(
                E_INVALIDARG, field.size() > c_maxSecurityFieldCharacters, "Security audit field exceeds its UTF-16 limit.");
            characters += field.size() + 1;
        }
        THROW_HR_IF_MSG(
            E_INVALIDARG, characters > c_maxSecurityPayloadCharacters, "Security audit exceeds its UTF-16 payload limit.");
    }

    void AppendAuditField(std::wstring& group, std::wstring_view label, std::wstring_view value, bool truncated = false)
    {
        const auto encoded = EscapeObservationPercents(std::wstring(value));
        THROW_HR_IF_MSG(E_INVALIDARG, encoded.size() > c_maxSecurityFieldCharacters, "Audit value exceeds its UTF-16 limit.");
        group += L"    ";
        group += label;
        group += L": ";
        group += encoded;
        if (truncated)
        {
            group += L" [truncated]";
        }
        // LF survives XML newline normalization without changing the insertion split.
        group += L"\n";
    }

    SecurityEventPayload GroupAuditPayload(
        DWORD eventId, const std::vector<std::wstring>& context, std::wstring linux, std::wstring operation, std::wstring timing, const Json& message)
    {
        std::wstring windows;
        AppendAuditField(windows, L"Distribution name", context.at(6));
        AppendAuditField(windows, L"Distribution ID", context.at(5));
        if (message.contains("wslcSessionId"))
        {
            THROW_HR_IF(
                E_INVALIDARG,
                !message.at("distroId").is_null() || !message.at("distroName").is_null() ||
                    (message.contains("wslSessionId") && !message.at("wslSessionId").is_null()));
            AppendAuditField(windows, L"WSLC session ID", Number(message, "wslcSessionId", UINT32_MAX));
            const auto name = Text(message, "wslcSessionName", c_maxWslcSessionNameBytes);
            THROW_HR_IF(E_INVALIDARG, name.empty());
            AppendAuditField(windows, L"WSLC session name", Wide(Json(name).dump()));
            const auto& id = message.at("containerId");
            const auto& containerName = message.at("containerName");
            THROW_HR_IF(E_INVALIDARG, id.is_null() != containerName.is_null());
            if (id.is_null())
            {
                AppendAuditField(windows, L"Container ID", L"Not available");
                AppendAuditField(windows, L"Container name", L"Not available");
            }
            else
            {
                const auto idText = Text(message, "containerId", 128);
                const auto nameText = Text(message, "containerName", 256);
                THROW_HR_IF(E_INVALIDARG, idText.empty() || nameText.empty());
                AppendAuditField(windows, L"Container ID", Wide(Json(idText).dump()));
                AppendAuditField(windows, L"Container name", Wide(Json(nameText).dump()));
            }
        }
        auto split = std::min(operation.size(), c_maxSecurityFieldCharacters);
        if (split < operation.size() && IS_HIGH_SURROGATE(operation[split - 1]) && IS_LOW_SURROGATE(operation[split]))
        {
            --split;
        }
        SecurityEventPayload result{};
        result.eventId = eventId;
        result.fields = {
            context.at(0), std::move(windows), std::move(linux), operation.substr(0, split), operation.substr(split), std::move(timing)};
        ValidateProcessPayload(result);
        return result;
    }

    struct ProcessDisplayField
    {
        const char* name;
        std::wstring_view label;
    };

    std::wstring ProcessGroup(const Json& data, std::initializer_list<ProcessDisplayField> fields)
    {
        std::wstring group;
        for (const auto& field : fields)
        {
            const auto& value = data.at(field.name);
            const bool decimalString =
                std::string_view(field.name) == "cgroupId" || std::string_view(field.name) == "creatorCgroupId";
            AppendAuditField(
                group,
                field.label,
                decimalString && value.at("status") == "present" ? Wide(value.at("value").get_ref<const std::string&>())
                : value.is_object()                              ? ReadableObservation(value)
                                                                 : Wide(value.dump()),
                value.is_object() && value.value("truncated", false));
        }
        return group;
    }

    bool IsGroupedMessage(std::wstring_view text)
    {
        for (const auto& [heading, insertion] :
             {std::pair<std::wstring_view, std::wstring_view>{L"Operation details", L"%4%5"},
              {L"Linux process and observed identity", L"%3"},
              {L"Windows and distribution", L"%2"},
              {L"Timing and collection", L"%6"},
              {L"Event format version:", L"%1"}})
        {
            const auto offset = text.find(heading);
            if (offset == std::wstring_view::npos)
            {
                return false;
            }
            const auto body = text.substr(offset + heading.size());
            const auto first = body.find_first_not_of(L" \t\r\n");
            if (first == std::wstring_view::npos || !body.substr(first).starts_with(insertion))
            {
                return false;
            }
        }
        for (const auto forbidden :
             {L"Data=", L"Windows user SID:", L"Capture session ID:", L"Policy source:", L"Windows received time"})
        {
            if (text.find(forbidden) != std::wstring_view::npos)
            {
                return false;
            }
        }
        std::array<size_t, 6> occurrences{};
        for (size_t index = 0; index + 1 < text.size(); ++index)
        {
            if (text[index] != L'%' || text[index + 1] < L'0' || text[index + 1] > L'9')
            {
                continue;
            }
            const auto insertion = text[++index] - L'0';
            if (insertion < 1 || insertion > 6 || (index + 1 < text.size() && text[index + 1] >= L'0' && text[index + 1] <= L'9'))
            {
                return false;
            }
            ++occurrences[insertion - 1];
        }
        return std::all_of(occurrences.begin(), occurrences.end(), [](size_t count) { return count == 1; });
    }

    bool HasPrivilege(HANDLE token, const LUID& luid)
    {
        const auto privileges = wil::get_token_information<TOKEN_PRIVILEGES>(token);
        for (DWORD index = 0; index < privileges->PrivilegeCount; ++index)
        {
            const auto& entry = privileges->Privileges[index];
            if (entry.Luid.LowPart == luid.LowPart && entry.Luid.HighPart == luid.HighPart)
            {
                return (entry.Attributes & SE_PRIVILEGE_REMOVED) == 0;
            }
        }
        return false;
    }

    void RequireApplicationGeneratedSuccess()
    {
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY),
            (security_detail::QueryApplicationGeneratedPolicy() & POLICY_AUDIT_EVENT_SUCCESS) == 0,
            "Security sink requires Application Generated success auditing. An administrator must explicitly enable it; the "
            "writer never changes policy.");
    }
} // namespace

void ValidateSecuritySourceName(std::wstring_view source)
{
    if (source == c_securitySourceName)
    {
        return;
    }
    const std::wstring_view prefix = source.starts_with(c_securityVerifySourcePrefix) ? c_securityVerifySourcePrefix : c_securityTestSourcePrefix;
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        !source.starts_with(prefix) || source.size() != prefix.size() + 32,
        "Security source must be the native WSL audit source, or its Test-/Verify- source with 32 lowercase hexadecimal digits.");
    for (const auto character : source.substr(prefix.size()))
    {
        THROW_HR_IF(E_INVALIDARG, !((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f')));
    }
}

static void ProjectHostFields(const Json& message, std::span<std::wstring, 7> fields)
{
    auto set = [&](SecurityProcessField field, std::wstring value) { fields[static_cast<size_t>(field)] = std::move(value); };
    set(SecurityProcessField::SchemaVersion, std::to_wstring(c_securitySchemaVersion));
    const auto sidText = Wide(Text(message, "windowsUserSid", 184));
    THROW_HR_IF_MSG(E_INVALIDARG, !sidText.starts_with(L"S-1-"), "Windows user SID must be a canonical SID, not an SDDL alias.");
    wil::unique_any<PSID, decltype(&::LocalFree), ::LocalFree> sid;
    THROW_HR_IF_MSG(E_INVALIDARG, !ConvertStringSidToSidW(sidText.c_str(), &sid), "Missing or invalid trusted Windows user SID.");
    THROW_HR_IF(E_INVALIDARG, !IsValidSid(sid.get()));
    set(SecurityProcessField::WindowsUserSid, sidText);
    set(SecurityProcessField::WslSessionId, OptionalSession(message, "wslSessionId"));
    set(SecurityProcessField::WindowsSessionId, OptionalSession(message, "windowsSessionId"));
    const auto attribution = Text(message, "attribution", 64);
    const bool known = attribution == "matched";
    const bool unknown = attribution == "unknown";
    THROW_HR_IF_MSG(E_INVALIDARG, !known && !unknown, "Invalid Windows distribution attribution.");
    set(SecurityProcessField::Attribution, Wide(attribution));
    if (known)
    {
        const auto guid = Text(message, "distroId", 38);
        THROW_HR_IF_MSG(E_INVALIDARG, !IsGuid(guid), "Known attribution requires a valid distribution GUID.");
        const auto name = Text(message, "distroName", 256);
        THROW_HR_IF(E_INVALIDARG, name.empty());
        set(SecurityProcessField::DistroGuid, Wide(guid));
        set(SecurityProcessField::DistroName, Wide(Json(name).dump()));
    }
}

static void SetProcessContext(SecurityEventPayload& result, const Json& message, const AuditPolicy& policy, uint32_t version)
{
    THROW_HR_IF(E_INVALIDARG, policy.source != "windows" && policy.source != "test-override");
    result.fields[0] = std::to_wstring(version);
    const auto capture = Text(message, "captureSessionId", 38);
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        capture.size() != 38 || capture.front() != '{' || !IsGuid(capture) || capture == "{00000000-0000-0000-0000-000000000000}",
        "Process audit requires a Windows-generated capture session GUID.");
    result.fields[7] = Wide(capture);
    const auto received = Text(message, "receivedUtc", 30);
    ValidateUtc(received);
    result.fields[8] = Wide(received);
    result.fields[9] = Wide(policy.source);
    if (result.fields[4] == L"unknown")
    {
        result.fields[5] = result.fields[6] = L"Not available";
    }
}

std::optional<SecurityProcessPayload> ProjectSecurityProcess(const Json& message, const AuditPolicy& policy)
{
    if (!policy.processCreation)
    {
        return {};
    }
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        !message.is_object() || !message.contains("version") || !message["version"].is_number_unsigned() ||
            message["version"].get<uint64_t>() != c_protocolVersion || !message.contains("kind") || !message["kind"].is_string(),
        "Invalid Windows Security audit envelope.");
    const auto kind = message["kind"].get<std::string>();
    if (kind == "ready" || kind == "stopped" || kind == "windows-status" || kind == "error")
    {
        return {};
    }
    THROW_HR_IF_MSG(E_INVALIDARG, kind != "process", "Only validated process messages are process audits.");
    THROW_HR_IF(E_INVALIDARG, policy.source != "windows" && policy.source != "test-override");
    SecurityProcessPayload result{};
    ProjectHostFields(message, std::span(result.fields).first<7>());
    auto set = [&](SecurityProcessField field, std::wstring value) {
        result.fields.at(static_cast<size_t>(field)) = std::move(value);
    };
    for (const auto& [field, name] :
         {std::pair{SecurityProcessField::Pid, "pid"},
          {SecurityProcessField::Tid, "tid"},
          {SecurityProcessField::Ppid, "ppid"},
          {SecurityProcessField::Uid, "uid"},
          {SecurityProcessField::Euid, "euid"},
          {SecurityProcessField::Gid, "gid"},
          {SecurityProcessField::Egid, "egid"},
          {SecurityProcessField::PidNamespace, "pidNamespace"}})
    {
        set(field, Number(message, name, UINT32_MAX));
    }
    set(SecurityProcessField::LinuxEventNs, Number(message, "timestampNs", UINT64_MAX));
    set(SecurityProcessField::LinuxStartNs, Number(message, "startTimeNs", UINT64_MAX));
    const auto executable = Text(message, "executable", 4096);
    const auto cwd = Text(message, "cwd", 4096);
    set(SecurityProcessField::Executable, Wide(Json(executable).dump()));
    set(SecurityProcessField::Cwd, Wide(Json(cwd).dump()));
    const auto executableMissing = Boolean(message, "executableMissing", executable.empty());
    const auto cwdMissing = Boolean(message, "cwdMissing", cwd.empty());
    set(SecurityProcessField::ExecutableMissing, executable.empty() ? L"true" : executableMissing);
    set(SecurityProcessField::CwdMissing, cwd.empty() ? L"true" : cwdMissing);
    const auto received = Text(message, "receivedUtc", 30);
    ValidateUtc(received);
    set(SecurityProcessField::ReceivedUtc, Wide(received));
    set(SecurityProcessField::CommandLineIncluded, policy.includeCommandLine ? L"true" : L"false");
    set(SecurityProcessField::CmdlineTruncated, Boolean(message, "cmdlineTruncated"));
    if (policy.includeCommandLine)
    {
        const auto iterator = message.find("argv");
        if (iterator != message.end())
        {
            THROW_HR_IF_MSG(E_INVALIDARG, !iterator->is_array(), "Invalid process argument array.");
            size_t bytes{};
            for (const auto& argument : *iterator)
            {
                THROW_HR_IF(E_INVALIDARG, !argument.is_string());
                const auto& value = argument.get_ref<const std::string&>();
                THROW_HR_IF_MSG(
                    E_INVALIDARG, value.size() >= 4096 - bytes, "Process arguments exceed the 4096-byte collection limit.");
                bytes += value.size() + 1;
                Wide(value);
            }
            set(SecurityProcessField::Argv, Wide(iterator->dump()));
        }
        set(SecurityProcessField::CmdlineMissing, Boolean(message, "cmdlineMissing", iterator == message.end()));
    }
    else
    {
        // Do not even validate or serialize guest argv when command-line auditing is disabled.
        set(SecurityProcessField::CmdlineMissing, L"not-recorded-by-policy");
    }
    set(SecurityProcessField::PolicySource, Wide(policy.source));
    size_t characters{};
    for (const auto& field : result.fields)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, field.size() > c_maxSecurityFieldCharacters, "Security field exceeds its encoded limit.");
        characters += field.size() + 1;
    }
    THROW_HR_IF_MSG(
        E_INVALIDARG, characters > c_maxSecurityPayloadCharacters, "Security payload exceeds its bounded UTF-16 limit.");
    return result;
}

std::optional<SecurityEventPayload> ProjectProcessExecuted(const Json& message, const AuditPolicy& policy)
{
    if (!policy.processCreation)
    {
        return {};
    }
    const auto data = ProcessExecutedData(message, policy.includeCommandLine);
    SecurityEventPayload result{};
    result.eventId = c_processExecutedEventId;
    ProjectHostFields(message, std::span(result.fields).first<7>());
    SetProcessContext(result, message, policy, AuditSchemaVersion(message, c_processExecutedSchemaVersion));
    return GroupAuditPayload(
        result.eventId,
        result.fields,
        ProcessGroup(
            data,
            {{"vmPid", L"Process ID"},
             {"vmTid", L"Thread ID"},
             {"vmParentPid", L"Parent process ID"},
             {"namespacePid", L"Process ID in PID namespace"},
             {"pidNamespace", L"PID namespace ID"},
             {"mountNamespace", L"Mount namespace ID"},
             {"networkNamespace", L"Network namespace ID"},
             {"cgroupId", L"Cgroup ID"},
             {"uid", L"User ID (UID)"},
             {"euid", L"Effective user ID (EUID)"},
             {"gid", L"Group ID (GID)"},
             {"egid", L"Effective group ID (EGID)"}}),
        ProcessGroup(data, {{"executable", L"Executable"}, {"cwd", L"Working directory"}, {"argv", L"Arguments (in order)"}}),
        ProcessGroup(data, {{"linuxEventNs", L"Linux event clock (monotonic ns)"}}),
        message);
}

std::optional<SecurityEventPayload> ProjectProcessCreated(const Json& message, const AuditPolicy& policy)
{
    if (!policy.processCreation)
    {
        return {};
    }
    const auto data = ProcessCreatedData(message);
    SecurityEventPayload result{};
    result.eventId = c_processCreatedEventId;
    ProjectHostFields(message, std::span(result.fields).first<7>());
    SetProcessContext(result, message, policy, AuditSchemaVersion(message, c_processCreatedSchemaVersion));
    return GroupAuditPayload(
        result.eventId,
        result.fields,
        ProcessGroup(
            data,
            {{"vmPid", L"Process ID (VM)"},
             {"vmTid", L"Thread ID (VM)"},
             {"namespacePid", L"Process ID in child PID namespace"},
             {"pidNamespace", L"Child PID namespace ID"},
             {"mountNamespace", L"Child mount namespace ID"},
             {"networkNamespace", L"Child network namespace ID"},
             {"creatorCgroupId", L"Creator cgroup ID"},
             {"uid", L"User ID (UID)"},
             {"euid", L"Effective user ID (EUID)"},
             {"gid", L"Group ID (GID)"},
             {"egid", L"Effective group ID (EGID)"}}),
        ProcessGroup(data, {{"vmCreatorPid", L"Creator process ID (VM)"}}),
        ProcessGroup(data, {{"linuxEventNs", L"Linux creation clock (monotonic ns)"}}),
        message);
}

std::optional<SecurityEventPayload> ProjectSecurityEvent(const Json& message, const AuditPolicy& policy)
{
    if (!policy.AnyEnabled())
    {
        return {};
    }
    THROW_HR_IF(E_INVALIDARG, policy.source != "windows" && policy.source != "test-override");
    const auto data = SelectSecurityEvent(message);
    if (!AllowsSecurityEvent(policy, data.at("family").get_ref<const std::string&>(), data.at("outcome").get_ref<const std::string&>()))
    {
        return {};
    }
    SecurityEventPayload result{};
    result.eventId = c_securityEventId;
    ProjectHostFields(message, std::span(result.fields).first<7>());
    result.fields[7] = Wide(data.at("family").get_ref<const std::string&>());
    result.fields[8] = Wide(data.at("operation").get_ref<const std::string&>());
    const auto received = Text(message, "receivedUtc", 30);
    ValidateUtc(received);
    result.fields[9] = Wide(received);
    result.fields[10] = Wide(data.dump());
    result.fields[11] = Wide(policy.source);
    size_t characters{};
    for (const auto& field : result.fields)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, field.size() > c_maxSecurityFieldCharacters, "Security field exceeds its encoded limit.");
        characters += field.size() + 1;
    }
    THROW_HR_IF_MSG(
        E_INVALIDARG, characters > c_maxSecurityPayloadCharacters, "Security payload exceeds its bounded UTF-16 limit.");
    return result;
}

std::optional<SecurityEventPayload> ProjectSecurityObservation(const Json& message, const AuditPolicy& policy)
{
    if (!policy.AnyEnabled())
    {
        return {};
    }
    ValidateSecurityEvent(message);
    if (!AllowsSecurityEvent(policy, message.at("family").get<std::string_view>(), message.at("outcome").get<std::string_view>()))
    {
        return {};
    }
    const auto data = SecurityObservationData(message);
    const auto& schema = GetObservationSchema(message.at("operation").get<std::string_view>());
    const auto operation = message.at("operation").get<std::string_view>();
    const auto fields = ObservationFields(message.at("family").get<std::string_view>(), operation);
    SecurityEventPayload result{};
    result.eventId = schema.eventId;
    ProjectHostFields(message, std::span(result.fields).first<7>());
    SetProcessContext(result, message, policy, AuditSchemaVersion(message, c_observationSchemaVersion));
    const auto format = [&](const ObservationField& field) {
        const auto& value = data.at(field.name);
        if (value.at("status") == "not_applicable")
        {
            return std::wstring(L"Not applicable");
        }
        if (value.at("status") == "unavailable")
        {
            return std::wstring(L"Not available");
        }
        if (field.type == ObservationType::Unsigned64 || field.type == ObservationType::Signed64)
        {
            return Wide(value.at("value").get_ref<const std::string&>());
        }
        return ReadableObservation(value);
    };
    std::wstring linux;
    std::wstring details;
    std::wstring timing;
    AppendAuditField(details, L"Linux operation", Wide(schema.operation));
    AppendAuditField(details, L"Linux outcome", Wide(data.at("outcome").get_ref<const std::string&>()));
    for (const auto& field : ObservationCommonFields(operation))
    {
        const std::string_view name = field.name;
        auto& group = name == "result" ? details : name == "linuxEventNs" ? timing : linux;
        AppendAuditField(group, Wide(field.label), format(field));
    }
    for (const auto& field : fields)
    {
        AppendAuditField(details, Wide(field.label), format(field));
    }
    return GroupAuditPayload(result.eventId, result.fields, std::move(linux), std::move(details), std::move(timing), message);
}

std::optional<Json> ProjectSecurityDiagnostic(const Json& message, const AuditPolicy& policy)
{
    if (!message.is_object() || !message.contains("kind") || !message["kind"].is_string())
    {
        return {};
    }
    const auto kind = message["kind"].get<std::string>();
    if (kind != "ready" && kind != "stopped" && kind != "windows-status" && kind != "error")
    {
        return {};
    }
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        !message.contains("version") || !message["version"].is_number_unsigned() || message["version"].get<uint64_t>() != c_protocolVersion,
        "Invalid Windows Security diagnostic envelope.");
    THROW_HR_IF(E_INVALIDARG, policy.source != "windows" && policy.source != "test-override");
    Json result = {{"version", c_protocolVersion}, {"kind", kind}, {"securityProcessAudit", false}, {"policySource", policy.source}};
    const auto copy = [&](const char* name, const auto& valid) {
        const auto found = message.find(name);
        if (found != message.end())
        {
            THROW_HR_IF_MSG(E_INVALIDARG, !valid(*found), "Invalid Security diagnostic field: %hs.", name);
            result[name] = *found;
        }
    };
    const auto boolean = [](const Json& value) { return value.is_boolean(); };
    const auto counter = [](const Json& value) { return value.is_number_unsigned(); };
    const auto uint32 = [](const Json& value) { return value.is_number_unsigned() && value.get<uint64_t>() <= UINT32_MAX; };
    const auto code = [](const Json& value) {
        if (value.is_number_unsigned())
        {
            return value.get<uint64_t>() <= INT32_MAX;
        }
        return value.is_number_integer() && value.get<int64_t>() >= INT32_MIN && value.get<int64_t>() <= INT32_MAX;
    };
    if (message.contains("receivedUtc"))
    {
        const auto received = Text(message, "receivedUtc", 30);
        ValidateUtc(received);
        result["receivedUtc"] = received;
    }
    if (message.contains("status"))
    {
        const auto status = Text(message, "status", 32);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            status != "awaiting-ready" && status != "policy-disabled" && status != "running" && status != "disabled" &&
                status != "complete",
            "Invalid Security diagnostic lifecycle status.");
        result["status"] = status;
    }
    if (kind == "windows-status")
    {
        copy("processCreation", boolean);
        copy("includeCommandLine", boolean);
        copy("policy", [&](const Json& value) { return value == PolicySettings(policy); });
    }
    else if (kind == "ready")
    {
        copy("pidNamespace", uint32);
        copy("collectionEnabled", boolean);
        copy("includeCommandLine", boolean);
        copy("initializeCode", code);
        copy("hooksAttachCode", code);
        copy("tracefsPrivateMount", boolean);
        const auto coverage = ExpectedCollectorCoverage(message);
        copy("coverage", [&](const Json& value) { return value == coverage; });
        copy("dnsProbeCount", uint32);
        copy("dnsCoverage", [](const Json& value) { return value == "unavailable" || value == "vm-root-only"; });
        copy("initializationDiagnostics", boolean);
        copy("policy", [&](const Json& value) { return value == PolicySettings(policy); });
        // Preserve the useful enrichment fact without echoing an arbitrary guest/library string.
        if (message.contains("loadedVariant"))
        {
            const auto variant = Text(message, "loadedVariant", 256);
            result["enrichedObjectLoaded"] = variant.ends_with("_btf_rbuff_enriched_fmap.o");
        }
    }
    else if (kind == "stopped")
    {
        copy("exitCode", uint32);
        for (const auto* name :
             {"processEvents",
              "securityEvents",
              "queueDropped",
              "perfLost",
              "invalidRecords",
              "ignoredRecords",
              "policyFiltered",
              "callbackFailures"})
        {
            copy(name, counter);
        }
        copy("pollError", code);
        copy("outputFailed", boolean);
        copy("kernelDropReasons", [](const Json& value) {
            return value.is_array() && value.size() <= 64 &&
                   std::all_of(value.begin(), value.end(), [](const Json& item) { return item.is_number_unsigned(); });
        });
        if (message.contains("reason"))
        {
            const auto reason = Text(message, "reason", 256);
            result["reason"] = reason == "stop" || reason == "duration" || reason == "eof" || reason == "error" ? reason : "unknown";
        }
    }
    else if (kind == "error")
    {
        const auto source = message.find("source");
        if (source != message.end() && source->is_string() && (*source == "windows-host" || *source == "windows-plugin"))
        {
            result["source"] = *source;
            const auto error = message.find("hresult");
            if (error != message.end())
            {
                THROW_HR_IF_MSG(
                    E_INVALIDARG, !(uint32(*error) || code(*error)), "Trusted Windows error HRESULT is not a 32-bit value.");
                result["hresult"] = error->is_number_unsigned() ? error->get<uint32_t>() : static_cast<uint32_t>(error->get<int32_t>());
            }
        }
    }
    return result;
}

namespace security_detail {
    wil::unique_handle PrimaryAuthority(PCWSTR privilege, bool required)
    {
        auto self = wil::run_as_self();
        wil::unique_handle primary;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary));
        wil::unique_handle duplicate;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            primary.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr, SecurityImpersonation, TokenImpersonation, &duplicate));
        LUID luid{};
        THROW_IF_WIN32_BOOL_FALSE(LookupPrivilegeValueW(nullptr, privilege, &luid));
        const bool present = HasPrivilege(duplicate.get(), luid);
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD),
            required && !present,
            "Security sink/setup requires an already-held %ls privilege. Elevation alone does not grant SeAuditPrivilege; no "
            "rights are assigned automatically.",
            privilege);
        if (present)
        {
            TOKEN_PRIVILEGES requested{};
            requested.PrivilegeCount = 1;
            requested.Privileges[0] = {luid, SE_PRIVILEGE_ENABLED};
            SetLastError(ERROR_SUCCESS);
            THROW_IF_WIN32_BOOL_FALSE(AdjustTokenPrivileges(duplicate.get(), FALSE, &requested, 0, nullptr, nullptr));
            THROW_IF_WIN32_ERROR(GetLastError());
        }
        return duplicate;
    }

    ULONG QueryApplicationGeneratedPolicy()
    {
        auto self = wil::run_as_self();
        auto authority = PrimaryAuthority(SE_SECURITY_NAME, false);
        auto impersonation = wil::impersonate_token(authority.get());
        auto guid = c_applicationGeneratedAudit;
        UniquePolicy policy;
        THROW_LAST_ERROR_IF_MSG(
            !AuditQuerySystemPolicy(&guid, 1, &policy),
            "Cannot read Application Generated auditing. The service needs existing policy-query access or SeSecurityPrivilege "
            "for this read; no rights are granted.");
        return policy.get()->AuditingInformation;
    }

    SourceInfo QuerySource(std::wstring_view source)
    {
        ValidateSecuritySourceName(source);
        auto self = wil::run_as_self();
        auto authority = PrimaryAuthority(SE_SECURITY_NAME, false);
        auto impersonation = wil::impersonate_token(authority.get());
        SourceInfo result{};
        const auto path = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Security\\") + std::wstring(source);
        wil::unique_hkey registry;
        result.registryStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE, &registry);
        if (result.registryStatus == ERROR_SUCCESS)
        {
            DWORD registryBytes{};
            result.registryMessageStatus =
                RegGetValueW(registry.get(), nullptr, L"EventMessageFile", RRF_RT_REG_SZ, nullptr, nullptr, &registryBytes);
            if (result.registryMessageStatus == ERROR_SUCCESS)
            {
                THROW_HR_IF(E_UNEXPECTED, registryBytes < sizeof(wchar_t) || registryBytes > 64 * 1024 || registryBytes % sizeof(wchar_t) != 0);
                std::vector<wchar_t> file(registryBytes / sizeof(wchar_t));
                result.registryMessageStatus =
                    RegGetValueW(registry.get(), nullptr, L"EventMessageFile", RRF_RT_REG_SZ, nullptr, file.data(), &registryBytes);
                if (result.registryMessageStatus == ERROR_SUCCESS)
                {
                    result.registryMessageFile.assign(file.data(), wcsnlen(file.data(), file.size()));
                }
            }
        }
        DWORD count{};
        DWORD bytes{};
        if (!AuthzEnumerateSecurityEventSources(0, nullptr, &count, &bytes))
        {
            const auto error = GetLastError();
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(error),
                error != ERROR_INSUFFICIENT_BUFFER,
                "Cannot enumerate Authz Security sources; check the already-held service/setup privileges.");
        }
        if (bytes == 0)
        {
            result.enumerationCount = count;
            return result;
        }
        THROW_HR_IF(E_UNEXPECTED, bytes > 16 * 1024 * 1024);
        std::vector<BYTE> buffer(bytes);
        auto* entries = reinterpret_cast<AUTHZ_SOURCE_SCHEMA_REGISTRATION*>(buffer.data());
        THROW_IF_WIN32_BOOL_FALSE(AuthzEnumerateSecurityEventSources(0, entries, &count, &bytes));
        result.enumerationCount = count;
        THROW_HR_IF(E_UNEXPECTED, count > buffer.size() / sizeof(AUTHZ_SOURCE_SCHEMA_REGISTRATION));
        for (DWORD index = 0; index < count; ++index)
        {
            const auto& entry = entries[index];
            if (entry.szEventSourceName && source == entry.szEventSourceName)
            {
                result.installed = true;
                result.flags = entry.dwFlags;
                result.messageFile = entry.szEventMessageFile ? entry.szEventMessageFile : L"";
                break;
            }
        }
        return result;
    }

    bool SourceReadbackMatches(const SourceInfo& source, std::wstring_view expectedMessageFile)
    {
        const auto samePath = [](std::wstring_view left, std::wstring_view right) {
            return left.size() == right.size() && left.find(L'\0') == std::wstring_view::npos &&
                   right.find(L'\0') == std::wstring_view::npos && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
        };
        return source.installed && source.registryStatus == ERROR_SUCCESS && source.registryMessageStatus == ERROR_SUCCESS &&
               !source.messageFile.empty() && samePath(source.messageFile, source.registryMessageFile) &&
               (expectedMessageFile.empty() || samePath(source.messageFile, expectedMessageFile));
    }

    void ValidateMessageResource(std::wstring_view path)
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            path.empty() || path.find(L'\0') != std::wstring_view::npos || !std::filesystem::path(path).is_absolute(),
            "MessageFile must be an existing absolute executable/DLL path containing the WSL Security resource.");
        const std::wstring file(path);
        const auto attributes = GetFileAttributesW(file.c_str());
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "The specified Security message file is unavailable.");
        wil::unique_hmodule module{LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
        THROW_LAST_ERROR_IF_MSG(!module, "Cannot load the Security message resource as data.");
        wil::unique_hlocal_string message;
        THROW_LAST_ERROR_IF_MSG(
            !FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                module.get(),
                c_securityProcessEventId,
                0,
                reinterpret_cast<PWSTR>(&message),
                0,
                nullptr),
            "MessageFile does not provide the WSL process event 62001.");
        const std::wstring_view text(message.get());
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            text.find(L"WSL Linux process Security audit") == std::wstring_view::npos ||
                text.find(L"SchemaVersion=%1") == std::wstring_view::npos || text.find(L"Argv=%22") == std::wstring_view::npos ||
                text.find(L"PolicySource=%27") == std::wstring_view::npos,
            "MessageFile has an unrelated or incompatible event 62001 schema.");
        message.reset();
        THROW_LAST_ERROR_IF_MSG(
            !FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                module.get(),
                c_securityEventId,
                0,
                reinterpret_cast<PWSTR>(&message),
                0,
                nullptr),
            "MessageFile does not provide the WSL security event 62002. Deploy matching plugin and message resources.");
        const std::wstring_view securityText(message.get());
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            securityText.find(L"WSL Linux security observation") == std::wstring_view::npos ||
                securityText.find(L"Family=%8") == std::wstring_view::npos || securityText.find(L"Data=%11") == std::wstring_view::npos ||
                securityText.find(L"PolicySource=%12") == std::wstring_view::npos,
            "MessageFile has an incompatible event 62002 schema.");
        message.reset();
        THROW_LAST_ERROR_IF_MSG(
            !FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                module.get(),
                c_processExecutedEventId,
                0,
                reinterpret_cast<PWSTR>(&message),
                0,
                nullptr),
            "MessageFile lacks ProcessExecuted event 62010. Deploy matching plugin and message resources.");
        const std::wstring_view executedText(message.get());
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            executedText.find(L"ProcessExecuted") == std::wstring_view::npos || !IsGroupedMessage(executedText),
            "MessageFile has an incompatible event 62010 schema.");
        message.reset();
        THROW_LAST_ERROR_IF_MSG(
            !FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                module.get(),
                c_processCreatedEventId,
                0,
                reinterpret_cast<PWSTR>(&message),
                0,
                nullptr),
            "MessageFile lacks ProcessCreated event 62011. Deploy matching plugin and message resources.");
        const std::wstring_view createdText(message.get());
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            createdText.find(L"ProcessCreated") == std::wstring_view::npos || !IsGroupedMessage(createdText),
            "MessageFile has an incompatible event 62011 schema.");
        for (const auto& schema : c_observationSchemas)
        {
            message.reset();
            THROW_LAST_ERROR_IF_MSG(
                !FormatMessageW(
                    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                    module.get(),
                    schema.eventId,
                    0,
                    reinterpret_cast<PWSTR>(&message),
                    0,
                    nullptr),
                "MessageFile lacks observation event %u.",
                schema.eventId);
            const std::wstring_view observationText(message.get());
            THROW_HR_IF_MSG(
                E_INVALIDARG,
                observationText.find(Wide(schema.eventType)) == std::wstring_view::npos || !IsGroupedMessage(observationText),
                "MessageFile has an incompatible observation schema.");
        }
    }
} // namespace security_detail

void WindowsSecuritySink::CloseProvider(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept
{
    LOG_IF_WIN32_BOOL_FALSE(AuthzUnregisterSecurityEventSource(0, &provider));
}

WindowsSecuritySink::WindowsSecuritySink(const AuditPolicy& policy, std::wstring source) :
    m_policy(policy), m_source(std::move(source))
{
    ValidateSecuritySourceName(m_source);
    if (!m_policy.AnyEnabled())
    {
        return;
    }
    m_authority = security_detail::PrimaryAuthority(SE_AUDIT_NAME);
    auto self = wil::run_as_self();
    auto impersonation = wil::impersonate_token(m_authority.get());
    RequireApplicationGeneratedSuccess();
    const auto installed = security_detail::QuerySource(m_source);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
        installed.installed && m_source == c_securitySourceName && !IsNativeSecuritySourceOwned(),
        "Native WSL Security audit source is missing its installer ownership marker; refusing to use a conflicting source.");
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !installed.installed,
        "Native WSL Security audit source is not installed. Repair the WSL installation.");
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        !security_detail::SourceReadbackMatches(installed),
        "Installed WSL Security source enumeration and native registry resource metadata do not agree.");
    security_detail::ValidateMessageResource(installed.messageFile);
    THROW_LAST_ERROR_IF_MSG(
        !AuthzRegisterSecurityEventSource(c_securitySourceRuntimeFlags, m_source.c_str(), &m_provider),
        "Cannot register the installed WSL Security source under process authority. No file fallback is performed.");
}

WindowsSecuritySink::~WindowsSecuritySink()
{
    if (m_provider)
    {
        try
        {
            auto self = wil::run_as_self();
            auto impersonation = wil::impersonate_token(m_authority.get());
            m_provider.reset();
        }
        catch (...)
        {
            LOG_IF_FAILED(wil::ResultFromCaughtException());
            m_provider.reset();
        }
    }
}

void WindowsSecuritySink::Write(const Json& message)
{
    const auto diagnostic = ProjectSecurityDiagnostic(message, m_policy);
    if (diagnostic)
    {
        // Diagnostics are not Security observations. Never fall back with event payloads.
        ReportAuditDiagnostic(*diagnostic);
        return;
    }
    if (!m_policy.AnyEnabled())
    {
        return;
    }
    const bool security = message.value("kind", std::string{}) == "security";
    const bool created = security && message.value("operation", std::string{}) == "process-created";
    auto payload = created ? ProjectProcessCreated(message, m_policy)
                           : (security ? ProjectSecurityObservation(message, m_policy) : ProjectProcessExecuted(message, m_policy));
    if (!payload)
    {
        return;
    }
    std::lock_guard lock(m_lock);
    auto self = wil::run_as_self();
    auto impersonation = wil::impersonate_token(m_authority.get());
    RequireApplicationGeneratedSuccess();
    auto& fields = payload->fields;
    THROW_HR_IF(E_UNEXPECTED, payload->eventId == 0 || fields.size() != 6);
    std::vector<AUDIT_PARAM> parameters(fields.size());
    for (size_t index = 0; index < fields.size(); ++index)
    {
        parameters[index].Type = APT_String;
        parameters[index].String = fields[index].data();
    }
    AUDIT_PARAMS audit{sizeof(AUDIT_PARAMS), APF_AuditSuccess, static_cast<USHORT>(parameters.size()), parameters.data()};
    // AuditSuccess means the observation was recorded; Linux outcome is in the operation group.
    // The Security record's actor is the Windows writer authority, never a guest-supplied SID.
    THROW_LAST_ERROR_IF_MSG(
        !AuthzReportSecurityEventFromParams(APF_AuditSuccess, m_provider.get(), payload->eventId, nullptr, &audit),
        "Authz rejected the WSL Security audit. API acceptance is not an OS durability guarantee; no fallback is "
        "performed.");
}

void WindowsSecuritySink::Flush()
{
    // Authz has no flush/durability API, and this sink has no private queue.
    std::lock_guard lock(m_lock);
}
} // namespace wsl::audit
