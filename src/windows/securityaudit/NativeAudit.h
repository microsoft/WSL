// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WindowsBridge.h"
#include <atomic>

namespace wsl::audit {

inline constexpr wchar_t c_legacyPluginName[] = L"WSL-SecurityAudit-Prototype";
inline constexpr unsigned c_maxCollectorAttempts = 3;
inline constexpr DWORD c_auditProcessTimeoutMs = 2000;
// Background collector initialization only; workloads never wait for this budget.
inline constexpr DWORD c_auditStartupTimeoutMs = 10000;

bool MachineAuditEnabled(std::optional<RegistryPolicyValue> value) noexcept;
bool QueryMachineAuditEnabled();
bool IsLegacyAuditPlugin(std::wstring_view name) noexcept;
void ReportAuditStatus(PCSTR stage, HRESULT result, unsigned attempt = 0) noexcept;
void ReportAuditDiagnostic(const nlohmann::json& diagnostic);

struct AuditTransport
{
    UniqueSocket input;
    UniqueSocket output;
    UniqueSocket error;

    SOCKET Input() const noexcept
    {
        return input ? input.get() : output.get();
    }
    void Shutdown() const noexcept;
};

// Factories must reject a stale VM generation and must not create or keep alive a VM.
// Arguments include argv[0]; executables are always the collector in WSL's boot image.
using AuditProcessFactory = std::function<AuditTransport(const std::vector<std::string>&)>;

struct NativeAuditServices
{
    std::function<bool()> enabled = QueryMachineAuditEnabled;
    std::function<AuditPolicy()> policy = QueryPolicy;
    std::function<std::shared_ptr<AuditSink>(const AuditPolicy&)> sink;
    // Tests may shorten, but never extend, the collector's overall initialization budget.
    DWORD startupTimeoutMs = c_auditStartupTimeoutMs;
};

class NativeAudit
{
    enum class StartupResult
    {
        Pending,
        Ready,
        Disabled,
        Failed,
        TimedOut,
        Cancelled
    };

    struct State;

public:
    explicit NativeAudit(NativeAuditServices services = {});
    NativeAudit(const NativeAudit&) = delete;
    NativeAudit& operator=(const NativeAudit&) = delete;
    ~NativeAudit();

    void Start(HostIdentity identity, AuditProcessFactory factory) noexcept;
    // Only signals cancellation. In particular, no joins, sends or COM calls under VM locks.
    void Stop() noexcept;
    // Diagnostic/test observation only. Stop and the destructor never wait on this handle.
    HANDLE WorkerHandle() const noexcept
    {
        return m_worker.get();
    }
    void DistributionStarted(DistributionIdentity identity) noexcept;
    void DistributionStopping(std::string_view guid) noexcept;
    void ContainerStarted(ContainerIdentity identity) noexcept;
    void ContainerStopping(std::string_view id) noexcept;

private:
    struct Work;
    static DWORD WINAPI Run(void* parameter) noexcept;
    static void Collect(const std::shared_ptr<State>& state, const AuditProcessFactory& factory);
    static void ResolveContainer(const std::shared_ptr<State>& state, ContainerIdentity identity, const AuditProcessFactory& factory);
    NativeAuditServices m_services;
    std::atomic<std::shared_ptr<State>> m_state;
    wil::unique_handle m_worker;
};

} // namespace wsl::audit
