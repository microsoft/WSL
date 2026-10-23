// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <wil/resource.h>
#include "Protocol.h"
#include "SecurityEvents.h"

namespace wsl::audit {

// WSLC session names can contain 255 UTF-16 code units, not just 255 UTF-8 bytes.
constexpr size_t c_maxWslcSessionNameBytes = 1024;

struct RegistryPolicyValue
{
    DWORD type{};
    DWORD value{};
};

struct WindowsAuditSettings
{
    ULONG processCreation{};
    ULONG network{};
    ULONG moduleLoad{};
    ULONG privilegeTransition{};
};

AuditPolicy EvaluatePolicy(const WindowsAuditSettings& settings, std::optional<RegistryPolicyValue> commandLineValue);
AuditPolicy QueryPolicy();
nlohmann::json ExpectedCollectorCoverage(const nlohmann::json& ready);
std::string ToUtf8(std::wstring_view value);
std::string SidString(PSID sid);
std::optional<DWORD> QueryTokenSessionId(HANDLE token);
std::string UtcNow();
std::string NewCaptureSessionId();

struct DistributionIdentity
{
    std::string name;
    std::string guid;
    uint64_t pidNamespace{};
};

struct HostIdentity
{
    std::string sid;
    std::optional<DWORD> wslSessionId;
    std::optional<DWORD> windowsSessionId;
    std::optional<DWORD> wslcSessionId{};
    std::string wslcSessionName{};
};

struct ContainerIdentity
{
    std::string id;
    std::string name;
    std::string startedAt;
    uint32_t initPid{};
    uint64_t pidNamespace{};
    uint64_t mountNamespace{};
    uint64_t observedAfterNs{};
};

class ProtocolError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Incremental byte framing; EOF is only clean between complete newline-terminated messages.
class NdjsonReceiver
{
public:
    void Feed(std::string_view bytes, const std::function<void(nlohmann::json)>& callback);
    void Finish() const;

private:
    std::string m_pending;
};

nlohmann::json ApplyWindowsPolicy(
    nlohmann::json event,
    const AuditPolicy& policy,
    const HostIdentity& host,
    const std::vector<DistributionIdentity>& distributions,
    const std::vector<ContainerIdentity>& containers = {},
    uint64_t rootPidNamespace = 0);

class AuditSink
{
public:
    virtual ~AuditSink() = default;
    virtual void Write(const nlohmann::json& message) = 0;
    virtual void Flush() = 0;
};

class EventSession
{
public:
    EventSession(AuditPolicy policy, HostIdentity host, std::shared_ptr<AuditSink> output);
    void SetDistributions(std::vector<DistributionIdentity> distributions);
    void SetContainers(std::vector<ContainerIdentity> containers);
    void Accept(nlohmann::json message);
    void Finish() const;
    bool Ready() const;
    bool Stopped() const;

private:
    AuditPolicy m_policy;
    HostIdentity m_host;
    std::shared_ptr<AuditSink> m_output;
    const std::string m_captureSessionId;
    mutable std::mutex m_lock;
    std::vector<DistributionIdentity> m_distributions;
    std::vector<ContainerIdentity> m_containers;
    uint64_t m_rootPidNamespace{};
    bool m_ready{};
    bool m_stopped{};
};

using UniqueSocket = wil::unique_socket;

} // namespace wsl::audit
