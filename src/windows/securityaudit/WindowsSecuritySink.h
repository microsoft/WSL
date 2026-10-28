// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WindowsBridge.h"
#include "ProcessExecuted.h"
#include "ProcessCreated.h"
#include "ObservationSchema.h"
#include <authz.h>

namespace wsl::audit {

inline constexpr wchar_t c_securitySourceName[] = L"WSL-SecurityAudit";
inline constexpr DWORD c_securityProcessEventId = 62001;
inline constexpr DWORD c_securityEventId = 62002;
inline constexpr DWORD c_securitySourceInstallFlags = AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES;
inline constexpr DWORD c_securitySourceRuntimeFlags = 0; // AuthzRegisterSecurityEventSource reserves its flags parameter.
inline constexpr uint32_t c_securitySchemaVersion = 1;
inline constexpr size_t c_securityFieldCount = 27;
// Conservative string-insertion budget verified with Authz on the test host, not a cross-version API guarantee.
inline constexpr size_t c_maxSecurityParameters = 28;
inline constexpr size_t c_maxSecurityFieldCharacters = 16 * 1024;
inline constexpr size_t c_maxSecurityPayloadCharacters = 24 * 1024;
inline constexpr GUID c_applicationGeneratedAudit = {0x0cce9222, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};

enum class SecurityProcessField : size_t
{
    SchemaVersion,
    WindowsUserSid,
    WslSessionId,
    WindowsSessionId,
    Attribution,
    DistroGuid,
    DistroName,
    Pid,
    Tid,
    Ppid,
    Executable,
    Cwd,
    Uid,
    Euid,
    Gid,
    Egid,
    PidNamespace,
    LinuxEventNs,
    LinuxStartNs,
    ReceivedUtc,
    CommandLineIncluded,
    Argv,
    CmdlineTruncated,
    ExecutableMissing,
    CwdMissing,
    CmdlineMissing,
    PolicySource
};

struct SecurityProcessPayload
{
    // DistroName/Executable/Cwd are JSON string literals; Argv is a JSON array or empty
    // when not recorded. Numeric insertions are decimal strings, preserving uint64_t clocks.
    std::array<std::wstring, c_securityFieldCount> fields;
    const std::wstring& Get(SecurityProcessField field) const
    {
        return fields.at(static_cast<size_t>(field));
    }
};

struct SecurityEventPayload
{
    std::vector<std::wstring> fields = std::vector<std::wstring>(12);
    DWORD eventId{};
};

inline constexpr size_t c_processExecutedFieldCount = 6;
inline constexpr size_t c_processCreatedFieldCount = 6;

std::optional<SecurityEventPayload> ProjectSecurityEvent(const nlohmann::json& message, const AuditPolicy& policy);
std::optional<SecurityEventPayload> ProjectProcessExecuted(const nlohmann::json& message, const AuditPolicy& policy);
std::optional<SecurityEventPayload> ProjectProcessCreated(const nlohmann::json& message, const AuditPolicy& policy);
std::optional<SecurityEventPayload> ProjectSecurityObservation(const nlohmann::json& message, const AuditPolicy& policy);

// Input is the Windows-enriched output of ApplyWindowsPolicy, never a raw guest record.
// Host fields: windowsUserSid, optional wslSessionId/windowsSessionId, attribution,
// distroId/distroName for known attribution, and receivedUtc.
std::optional<SecurityProcessPayload> ProjectSecurityProcess(const nlohmann::json& message, const AuditPolicy& policy);
// Diagnostic-only projection. EventSession rejects raw guest error frames; HRESULTs
// are accepted only from the direct Windows host/plugin error producers.
std::optional<nlohmann::json> ProjectSecurityDiagnostic(const nlohmann::json& message, const AuditPolicy& policy);
void ValidateSecuritySourceName(std::wstring_view source);

namespace security_detail {
    wil::unique_handle PrimaryAuthority(PCWSTR privilege, bool required = true);
    ULONG QueryApplicationGeneratedPolicy();
    constexpr ULONG PolicySetFlags(ULONG flags)
    {
        return flags == 0 ? POLICY_AUDIT_EVENT_NONE : flags;
    }

    struct SourceInfo
    {
        bool installed{};
        // Raw enumeration metadata, not an authoritative multi-instance capability query.
        DWORD flags{};
        std::wstring messageFile;
        DWORD enumerationCount{};
        LONG registryStatus{ERROR_FILE_NOT_FOUND};
        LONG registryMessageStatus{ERROR_FILE_NOT_FOUND};
        std::wstring registryMessageFile;
    };

    SourceInfo QuerySource(std::wstring_view source);
    bool SourceReadbackMatches(const SourceInfo& source, std::wstring_view expectedMessageFile = {});
    void ValidateMessageResource(std::wstring_view path);
} // namespace security_detail

class WindowsSecuritySink final : public AuditSink
{
public:
    explicit WindowsSecuritySink(const AuditPolicy& policy);
    ~WindowsSecuritySink() override;
    WindowsSecuritySink(const WindowsSecuritySink&) = delete;
    WindowsSecuritySink& operator=(const WindowsSecuritySink&) = delete;
    void Write(const nlohmann::json& message) override;
    void Flush() override;

private:
    static void CloseProvider(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept;
    using UniqueProvider = wil::unique_any<AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE, decltype(&CloseProvider), CloseProvider>;

    AuditPolicy m_policy;
    wil::unique_handle m_authority;
    UniqueProvider m_provider;
    std::mutex m_lock;
};

} // namespace wsl::audit
