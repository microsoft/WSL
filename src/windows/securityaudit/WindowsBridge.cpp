// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WindowsBridge.h"
#include <objbase.h>

namespace wsl::audit {

std::string NewCaptureSessionId()
{
    GUID id{};
    THROW_IF_FAILED(CoCreateGuid(&id));
    wchar_t text[39]{};
    THROW_HR_IF(E_UNEXPECTED, StringFromGUID2(id, text, ARRAYSIZE(text)) == 0);
    return ToUtf8(text);
}

AuditPolicy EvaluatePolicy(const WindowsAuditSettings& settings, std::optional<RegistryPolicyValue> commandLineValue)
{
    if (commandLineValue && commandLineValue->type != REG_DWORD)
    {
        throw std::invalid_argument("Command-line audit policy must be REG_DWORD");
    }

    for (const auto flags : {settings.processCreation, settings.network, settings.moduleLoad, settings.privilegeTransition})
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            (flags & ~(POLICY_AUDIT_EVENT_SUCCESS | POLICY_AUDIT_EVENT_FAILURE | POLICY_AUDIT_EVENT_NONE)) != 0 ||
                ((flags & POLICY_AUDIT_EVENT_NONE) != 0 && flags != POLICY_AUDIT_EVENT_NONE),
            "Invalid Windows audit policy flags.");
    }
    const bool creation = (settings.processCreation & POLICY_AUDIT_EVENT_SUCCESS) != 0;
    return {
        creation,
        creation && commandLineValue && commandLineValue->value == 1,
        "windows",
        (settings.network & POLICY_AUDIT_EVENT_SUCCESS) != 0,
        (settings.network & POLICY_AUDIT_EVENT_FAILURE) != 0,
        (settings.moduleLoad & POLICY_AUDIT_EVENT_SUCCESS) != 0,
        (settings.privilegeTransition & POLICY_AUDIT_EVENT_SUCCESS) != 0};
}

AuditPolicy QueryPolicy()
{
    GUID subcategories[] = {
        {0x0cce922b, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}},
        {0x0cce9226, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}},
        {0x0cce9211, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}},
        {0x0cce9231, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}}};
    wil::unique_any<PAUDIT_POLICY_INFORMATION, decltype(&::AuditFree), ::AuditFree> information;
    THROW_LAST_ERROR_IF_MSG(
        !AuditQuerySystemPolicy(subcategories, static_cast<ULONG>(std::size(subcategories)), &information),
        "Cannot read Windows audit policy; collection will not be enabled.");
    for (size_t index = 0; index < std::size(subcategories); ++index)
    {
        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            !IsEqualGUID(information.get()[index].AuditSubCategoryGuid, subcategories[index]),
            "Windows returned an unexpected audit subcategory.");
    }
    const WindowsAuditSettings settings{
        information.get()[0].AuditingInformation,
        information.get()[1].AuditingInformation,
        information.get()[2].AuditingInformation,
        information.get()[3].AuditingInformation};
    wil::unique_hkey key;
    const auto result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\Audit", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put());
    if (result == ERROR_FILE_NOT_FOUND)
    {
        return EvaluatePolicy(settings, std::nullopt);
    }
    THROW_IF_WIN32_ERROR(result);

    RegistryPolicyValue value{};
    DWORD size = sizeof(value.value);
    const auto query = RegQueryValueExW(
        key.get(), L"ProcessCreationIncludeCmdLine_Enabled", nullptr, &value.type, reinterpret_cast<BYTE*>(&value.value), &size);
    if (query == ERROR_FILE_NOT_FOUND)
    {
        return EvaluatePolicy(settings, std::nullopt);
    }
    THROW_IF_WIN32_ERROR(query);
    THROW_HR_IF(E_INVALIDARG, size != sizeof(value.value));
    return EvaluatePolicy(settings, value);
}

std::string ToUtf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    THROW_HR_IF(E_INVALIDARG, value.size() > INT_MAX);
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(size == 0);
    std::string result(size, '\0');
    THROW_LAST_ERROR_IF(
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0);
    return result;
}

std::wstring ToWide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    THROW_HR_IF(E_INVALIDARG, value.size() > INT_MAX);
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(size == 0);
    std::wstring result(size, L'\0');
    THROW_LAST_ERROR_IF(
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == 0);
    return result;
}

std::string SidString(PSID sid)
{
    THROW_HR_IF(E_INVALIDARG, !sid || !IsValidSid(sid));
    wil::unique_hlocal_string text;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(sid, text.put()));
    return ToUtf8(text.get());
}

std::string CurrentUserSid()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));
    DWORD size{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    THROW_LAST_ERROR_IF(GetLastError() != ERROR_INSUFFICIENT_BUFFER);
    std::vector<BYTE> buffer(size);
    THROW_IF_WIN32_BOOL_FALSE(GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size));
    return SidString(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}

std::optional<DWORD> QueryTokenSessionId(HANDLE token)
{
    if (!token)
    {
        return std::nullopt;
    }
    DWORD id{};
    DWORD size{};
    THROW_IF_WIN32_BOOL_FALSE(GetTokenInformation(token, ::TokenSessionId, &id, sizeof(id), &size));
    return id;
}

std::string UtcNow()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char text[32]{};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return text;
}

std::wstring QuoteArgument(std::wstring_view value)
{
    if (!value.empty() && value.find_first_of(L" \t\r\n\v\f\"") == std::wstring_view::npos)
    {
        return std::wstring(value);
    }

    std::wstring result = L"\"";
    size_t slashes{};
    for (const auto character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'"')
        {
            result.append(slashes * 2 + 1, L'\\');
        }
        else
        {
            result.append(slashes, L'\\');
        }
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

void NdjsonReceiver::Feed(std::string_view bytes, const std::function<void(nlohmann::json)>& callback)
{
    for (const char byte : bytes)
    {
        if (byte == '\n')
        {
            // Reject invalid UTF-8 even in JSON whitespace or unknown fields.
            if (!m_pending.empty() &&
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, m_pending.data(), static_cast<int>(m_pending.size()), nullptr, 0) == 0)
            {
                throw ProtocolError("Invalid UTF-8 audit frame");
            }
            nlohmann::json message;
            try
            {
                message = ParseMessage(m_pending);
            }
            catch (...)
            {
                throw ProtocolError("Invalid JSON audit frame or protocol envelope");
            }
            m_pending.clear();
            callback(std::move(message));
        }
        else
        {
            if (m_pending.size() >= c_maxMessageSize)
            {
                throw ProtocolError("Audit frame exceeds 64 KiB");
            }
            m_pending.push_back(byte);
        }
    }
}

void NdjsonReceiver::Finish() const
{
    if (!m_pending.empty())
    {
        throw ProtocolError("Audit stream ended in a partial frame");
    }
}

nlohmann::json ApplyWindowsPolicy(
    nlohmann::json event,
    const AuditPolicy& policy,
    const HostIdentity& host,
    const std::vector<DistributionIdentity>& distributions,
    const std::vector<ContainerIdentity>& containers,
    uint64_t rootPidNamespace)
{
    bool security = false;
    try
    {
        security = event.value("kind", std::string{}) == "security";
        if (security)
        {
            ValidateSecurityEvent(event);
        }
        else
        {
            ValidateProcessEvent(event);
        }
    }
    catch (...)
    {
        throw ProtocolError("Invalid audit event fields");
    }
    if (security ? !AllowsSecurityEvent(
                       policy, event.at("family").get_ref<const std::string&>(), event.at("outcome").get_ref<const std::string&>())
                 : !policy.processCreation)
    {
        return nullptr;
    }
    // Never forward untrusted extensions, including hidden command lines and host identity claims.
    auto validated = security ? SelectSecurityEvent(event) : nlohmann::json::object();
    if (!security)
    {
        for (const auto* name :
             {"version",
              "kind",
              "pid",
              "tid",
              "ppid",
              "pidNamespace",
              "uid",
              "euid",
              "gid",
              "egid",
              "timestampNs",
              "startTimeNs",
              "executable",
              "cwd",
              "cmdlineTruncated"})
        {
            validated[name] = event[name];
        }
        for (const auto* name : c_processBooleanFields)
        {
            if (event.contains(name))
            {
                validated[name] = event[name];
            }
        }
        for (const auto* name : {"namespacePid", "mountNamespace", "networkNamespace", "cgroupId"})
        {
            if (event.contains(name))
            {
                validated[name] = event[name];
            }
        }
    }
    if (!security && policy.includeCommandLine && event.contains("argv"))
    {
        validated["argv"] = event["argv"];
    }
    event = std::move(validated);
    if (!policy.includeCommandLine)
    {
        event.erase("argv");
    }
    // Never trust Linux-supplied attribution or wall-clock time.
    event["receivedUtc"] = UtcNow();
    event["policySource"] = policy.source;
    event["windowsUserSid"] = host.sid;
    event["wslSessionId"] = host.wslSessionId ? nlohmann::json(*host.wslSessionId) : nlohmann::json(nullptr);
    event["windowsSessionId"] = host.windowsSessionId ? nlohmann::json(*host.windowsSessionId) : nlohmann::json(nullptr);
    event["attribution"] = "unknown";
    event["distroName"] = nullptr;
    event["distroId"] = nullptr;
    const auto ns = event["pidNamespace"].get<uint64_t>();
    if (host.wslcSessionId)
    {
        event["wslSessionId"] = nullptr;
        event["wslcSessionId"] = *host.wslcSessionId;
        event["wslcSessionName"] = host.wslcSessionName;
        event["containerId"] = nullptr;
        event["containerName"] = nullptr;
        const ContainerIdentity* match{};
        const auto mount = event.find("mountNamespace");
        const bool haveMount = mount != event.end() && mount->is_number_unsigned() && *mount != 0;
        for (const auto& container : containers)
        {
            if (ns == 0 || ns != container.pidNamespace || container.mountNamespace == 0 ||
                (container.observedAfterNs != 0 &&
                 (!event.contains("timestampNs") || event["timestampNs"].get<uint64_t>() < container.observedAfterNs)) ||
                (haveMount ? mount->get<uint64_t>() != container.mountNamespace : ns == rootPidNamespace))
            {
                continue;
            }
            if (match)
            {
                // Shared namespaces do not identify one container.
                return event;
            }
            match = &container;
        }
        if (match)
        {
            event["containerId"] = match->id;
            event["containerName"] = match->name;
        }
        return event;
    }
    const DistributionIdentity* match{};
    for (const auto& distribution : distributions)
    {
        if (ns == distribution.pidNamespace && ns != 0)
        {
            if (match)
            {
                return event;
            }
            match = &distribution;
        }
    }
    if (match)
    {
        event["attribution"] = "matched";
        event["distroName"] = match->name;
        event["distroId"] = match->guid;
    }
    return event;
}

EventSession::EventSession(AuditPolicy policy, HostIdentity host, std::shared_ptr<AuditSink> output) :
    m_policy(std::move(policy)), m_host(std::move(host)), m_output(std::move(output)), m_captureSessionId(NewCaptureSessionId())
{
    m_output->Write(
        {{"version", c_protocolVersion},
         {"kind", "windows-status"},
         {"status", m_policy.AnyEnabled() ? "awaiting-ready" : "policy-disabled"},
         {"processCreation", m_policy.processCreation},
         {"includeCommandLine", m_policy.processCreation && m_policy.includeCommandLine},
         {"policy", PolicySettings(m_policy)},
         {"policySource", m_policy.source},
         {"captureSessionId", m_captureSessionId},
         {"receivedUtc", UtcNow()}});
}

void EventSession::SetDistributions(std::vector<DistributionIdentity> distributions)
{
    std::lock_guard lock(m_lock);
    m_distributions = std::move(distributions);
}

void EventSession::SetHostDistribution(DistributionIdentity distribution)
{
    std::lock_guard lock(m_lock);
    m_hostDistribution = std::move(distribution);
}

void EventSession::SetContainers(std::vector<ContainerIdentity> containers)
{
    std::lock_guard lock(m_lock);
    m_containers = std::move(containers);
}

nlohmann::json ExpectedCollectorCoverage(const nlohmann::json& ready)
{
    auto coverage = LibraryCoverage();
    if (ready.contains("dnsProbeCount") || ready.contains("dnsCoverage"))
    {
        const auto count = ready.find("dnsProbeCount");
        const auto status = ready.find("dnsCoverage");
        if (count == ready.end() || !count->is_number_unsigned() || count->get<uint64_t>() > UINT32_MAX ||
            status == ready.end() || *status != (*count == 0 ? "unavailable" : "vm-root-only"))
        {
            throw ProtocolError("Invalid DNS coverage diagnostic");
        }
        if (*count == 0)
        {
            auto& supported = coverage["network"]["supported"];
            supported.erase(std::remove(supported.begin(), supported.end(), "dns-query"), supported.end());
        }
    }
    return coverage;
}

static nlohmann::json ValidateControlFields(const nlohmann::json& message, bool ready)
{
    auto fields = nlohmann::json::object();
    const auto copy = [&](const char* name, const auto& valid) {
        const auto found = message.find(name);
        if (found != message.end())
        {
            if (!valid(*found))
            {
                throw ProtocolError(std::string("Invalid collector control field: ") + name);
            }
            fields[name] = *found;
        }
    };
    const auto boolean = [](const auto& value) { return value.is_boolean(); };
    const auto boundedString = [](const auto& value) {
        return value.is_string() && value.template get_ref<const std::string&>().size() <= 256;
    };
    const auto unsignedCounter = [](const auto& value) { return value.is_number_unsigned(); };
    const auto signedCode = [](const auto& value) {
        if (value.is_number_unsigned())
        {
            return value.template get<uint64_t>() <= INT32_MAX;
        }
        return value.is_number_integer() && value.template get<int64_t>() >= INT32_MIN && value.template get<int64_t>() <= INT32_MAX;
    };
    if (ready)
    {
        fields["pidNamespace"] = message.at("pidNamespace");
        copy("collectionEnabled", boolean);
        copy("includeCommandLine", boolean);
        copy("loadedVariant", boundedString);
        copy("initializeCode", signedCode);
        copy("hooksAttachCode", signedCode);
        copy("tracefsPrivateMount", boolean);
        const auto coverage = ExpectedCollectorCoverage(message);
        copy("coverage", [&](const auto& value) { return value == coverage; });
        copy("dnsProbeCount", [](const auto& value) {
            return value.is_number_unsigned() && value.template get<uint64_t>() <= UINT32_MAX;
        });
        copy("dnsCoverage", [](const auto& value) { return value == "unavailable" || value == "vm-root-only"; });
        copy("initializationDiagnostics", boolean);
        copy("policy", [](const auto& value) { return value.is_object(); });
    }
    else
    {
        fields["exitCode"] = message.at("exitCode");
        copy("reason", boundedString);
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
            copy(name, unsignedCounter);
        }
        copy("pollError", signedCode);
        copy("outputFailed", boolean);
        copy("kernelDropReasons", [](const auto& value) {
            return value.is_array() && value.size() <= 64 &&
                   std::all_of(value.begin(), value.end(), [](const auto& counter) { return counter.is_number_unsigned(); });
        });
    }
    return fields;
}

void EventSession::Accept(nlohmann::json message)
{
    std::lock_guard lock(m_lock);
    const auto kind = message.at("kind").get<std::string>();
    if (m_stopped)
    {
        throw std::invalid_argument("Message received after STOPPED");
    }
    auto control = nlohmann::json::object();
    if (kind == "ready")
    {
        if (m_ready || !message.contains("pidNamespace") || !message["pidNamespace"].is_number_unsigned() ||
            message["pidNamespace"].get<uint64_t>() == 0 || message["pidNamespace"].get<uint64_t>() > UINT32_MAX ||
            !message.contains("status") || !message["status"].is_string())
        {
            throw std::invalid_argument("Invalid or duplicate READY");
        }
        const auto status = message["status"].get<std::string>();
        if (status != (m_policy.AnyEnabled() ? "running" : "disabled"))
        {
            throw std::invalid_argument("Collector READY does not match configured policy");
        }
        control = ValidateControlFields(message, true);
        if ((control.contains("collectionEnabled") && control["collectionEnabled"] != m_policy.AnyEnabled()) ||
            (control.contains("includeCommandLine") && control["includeCommandLine"] != (m_policy.processCreation && m_policy.includeCommandLine)) ||
            !control.contains("policy") || control["policy"] != PolicySettings(m_policy))
        {
            throw ProtocolError("Collector READY control fields do not match configured Windows policy");
        }
        m_ready = true;
        m_rootPidNamespace = message["pidNamespace"].get<uint64_t>();
        if (m_hostDistribution)
        {
            m_hostDistribution->pidNamespace = message["pidNamespace"].get<uint64_t>();
            m_distributions = {*m_hostDistribution};
        }
    }
    else if (kind == "process" || kind == "security")
    {
        if (!m_ready)
        {
            throw std::invalid_argument("Process event before READY");
        }
        auto event = ApplyWindowsPolicy(std::move(message), m_policy, m_host, m_distributions, m_containers, m_rootPidNamespace);
        if (!event.is_null())
        {
            event["captureSessionId"] = m_captureSessionId;
            m_output->Write(event);
        }
        return;
    }
    else if (kind == "stopped")
    {
        if (!m_ready || !message.contains("exitCode") || !message["exitCode"].is_number_unsigned() || message["exitCode"].get<uint64_t>() != 0)
        {
            throw std::invalid_argument("Collector stopped unsuccessfully or before READY");
        }
        control = ValidateControlFields(message, false);
        m_stopped = true;
    }
    else if (kind == "error")
    {
        // Do not relay untrusted error fields: they could contain sensitive argv.
        throw std::runtime_error("Linux collector reported an explicit error; inspect its separately relayed stderr");
    }
    else
    {
        throw std::invalid_argument("Unexpected collector message kind");
    }
    // Only persist validated control fields, not arbitrary untrusted extensions.
    control["version"] = c_protocolVersion;
    control["kind"] = kind;
    control["status"] = kind == "ready" ? message["status"] : nlohmann::json("complete");
    control["receivedUtc"] = UtcNow();
    control["policySource"] = m_policy.source;
    control["captureSessionId"] = m_captureSessionId;
    m_output->Write(control);
}

void EventSession::Finish() const
{
    std::lock_guard lock(m_lock);
    if (!m_ready || !m_stopped)
    {
        throw std::runtime_error("Collector stream ended without READY and STOPPED");
    }
}

bool EventSession::Ready() const
{
    std::lock_guard lock(m_lock);
    return m_ready;
}

bool EventSession::Stopped() const
{
    std::lock_guard lock(m_lock);
    return m_stopped;
}

void WriteAll(HANDLE handle, std::string_view bytes)
{
    while (!bytes.empty())
    {
        DWORD written{};
        THROW_IF_WIN32_BOOL_FALSE(
            WriteFile(handle, bytes.data(), static_cast<DWORD>(std::min<size_t>(bytes.size(), MAXDWORD)), &written, nullptr));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), written == 0);
        bytes.remove_prefix(written);
    }
}

void SendAll(SOCKET socket, std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto written = send(socket, bytes.data(), static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX)), 0);
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), written == SOCKET_ERROR);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), written == 0);
        bytes.remove_prefix(written);
    }
}

} // namespace wsl::audit
