// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WindowsBridge.h"
#include "Launch.h"
#include <atomic>

namespace wsl::audit {

inline constexpr wchar_t c_legacyPluginName[] = L"WSL-SecurityAudit-Prototype";
inline constexpr DWORD c_auditIoTimeoutMs = 2000;
// Background collector initialization only; workloads never wait for this budget.
inline constexpr DWORD c_auditStartupTimeoutMs = 10000;

bool MachineAuditEnabled(std::optional<RegistryPolicyValue> value) noexcept;
bool QueryMachineAuditEnabled();
bool IsLegacyAuditPlugin(std::wstring_view name) noexcept;
void ReportAuditStatus(PCSTR stage, HRESULT result) noexcept;
void ReportAuditDiagnostic(const nlohmann::json& diagnostic);

class NativeAudit
{
    struct State;

public:
    // One context per VM, with policy captured at construction before VM creation.
    explicit NativeAudit(HostIdentity identity) noexcept;
    NativeAudit(const NativeAudit&) = delete;
    NativeAudit& operator=(const NativeAudit&) = delete;
    ~NativeAudit();

    // Captured policy, not a claim that the guest has finished initializing.
    bool Enabled() const noexcept
    {
        return m_state != nullptr;
    }
    // The VM owner binds this listener before boot. Accept one guest, without retries.
    void Start(UniqueSocket listener, DWORD bootTimeoutMs) noexcept;
    // Only signals cancellation. In particular, no joins, sends or COM calls under VM locks.
    void Stop() noexcept;
    void DistributionStarted(DistributionIdentity identity) noexcept;
    void DistributionStopping(std::string_view guid) noexcept;
    void ContainerStarted(ContainerIdentity identity) noexcept;
    void ContainerStopping(std::string_view id) noexcept;

private:
    static DWORD WINAPI Run(void* parameter) noexcept;
    static void Collect(const std::shared_ptr<State>& state, UniqueSocket socket);
    static void ApplyContainerInspection(State& state, const nlohmann::json& message);
    std::shared_ptr<State> m_state;
};

} // namespace wsl::audit
