// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WindowsBridge.h"
#include "ProcessExecuted.h"
#include "ProcessCreated.h"
#include "ObservationSchema.h"
#include <authz.h>

namespace wsl::audit {

inline constexpr wchar_t c_securitySourceName[] = L"Microsoft-Windows-WSL-SecurityAudit";
inline constexpr DWORD c_securitySourceInstallFlags = AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES;
inline constexpr DWORD c_securitySourceRuntimeFlags = 0; // AuthzRegisterSecurityEventSource reserves its flags parameter.
// Conservative string-insertion budget verified with Authz on the test host, not a cross-version API guarantee.
inline constexpr size_t c_maxSecurityParameters = 28;
inline constexpr size_t c_maxSecurityFieldCharacters = 16 * 1024;
inline constexpr size_t c_maxSecurityPayloadCharacters = 24 * 1024;
inline constexpr GUID c_applicationGeneratedAudit = {0x0cce9222, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};

struct SecurityEventPayload
{
    // Host/context fields are regrouped into six event insertions before writing.
    std::vector<std::wstring> fields = std::vector<std::wstring>(10);
    DWORD eventId{};
};

inline constexpr size_t c_processExecutedFieldCount = 6;
inline constexpr size_t c_processCreatedFieldCount = 6;

// Input is Windows-enriched output of ApplyWindowsPolicy, never a raw guest record.
std::optional<SecurityEventPayload> ProjectProcessExecuted(const nlohmann::json& message, const AuditPolicy& policy);
std::optional<SecurityEventPayload> ProjectProcessCreated(const nlohmann::json& message, const AuditPolicy& policy);
std::optional<SecurityEventPayload> ProjectSecurityObservation(const nlohmann::json& message, const AuditPolicy& policy);

// Diagnostic-only projection. EventSession rejects raw guest error frames; HRESULTs
// are accepted only from direct Windows host error producers.
std::optional<nlohmann::json> ProjectSecurityDiagnostic(const nlohmann::json& message, const AuditPolicy& policy);

namespace security_detail {
    wil::unique_handle PrimaryAuthority(PCWSTR privilege, bool required = true);
    ULONG QueryApplicationGeneratedPolicy();
    struct SourceInfo
    {
        bool installed{};
        std::wstring messageFile;
        LONG registryStatus{ERROR_FILE_NOT_FOUND};
        LONG registryMessageStatus{ERROR_FILE_NOT_FOUND};
        std::wstring registryMessageFile;
    };

    SourceInfo QuerySource();
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
