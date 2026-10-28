// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#pragma warning(push)
#pragma warning(disable : 4875)
#include <gsl/span>
#pragma warning(pop)
#include <source_location>

#include "NativeAudit.h"
#include "WindowsSecuritySink.h"
#include "WslTelemetry.h"
#include "socket.hpp"
#include <wil/token_helpers.h>

namespace wsl::audit {
namespace {
    HRESULT CurrentAuditFailure() noexcept
    {
        try
        {
            throw;
        }
        catch (const wil::ResultException& error)
        {
            return error.GetErrorCode();
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (...)
        {
            // Guest protocol exceptions must not expose their input or trigger WIL's debug fail-fast policy.
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
    }

    void ConfigureSocket(SOCKET socket)
    {
        u_long nonblocking = 1;
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR);
    }

    void SendControl(SOCKET socket, const nlohmann::json& message)
    {
        const auto bytes = EncodeMessage(message);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_auditIoTimeoutMs);
        size_t offset{};
        while (offset != bytes.size())
        {
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::chrono::steady_clock::now() >= deadline);
            fd_set writable{};
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            timeval timeout{0, 100000};
            const auto ready = select(0, nullptr, &writable, nullptr, &timeout);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
            if (ready == 0)
            {
                continue;
            }
            const auto count = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            {
                continue;
            }
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE), count == 0);
            offset += count;
        }
    }

    bool Read(SOCKET socket, const std::function<void(std::string_view)>& consume)
    {
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval timeout{0, 200000};
        const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
        if (ready != 0)
        {
            std::array<char, 8192> bytes{};
            const auto count = recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
            if (count == 0)
            {
                return false;
            }
            consume(std::string_view(bytes.data(), count));
        }
        return true;
    }
} // namespace

void ReportAuditStatus(PCSTR stage, HRESULT result) noexcept
{
    if (g_hTraceLoggingProvider)
    {
        WSL_LOG("NativeAuditStatus", TraceLoggingString(stage, "Stage"), TraceLoggingHResult(result, "Result"));
    }
    LOG_IF_FAILED_MSG(result, "Native WSL Security audit: %hs. Workload startup remains available.", stage);
}

void ReportAuditDiagnostic(const nlohmann::json& diagnostic)
{
    const auto text = diagnostic.dump();
    if (g_hTraceLoggingProvider)
    {
        WSL_LOG("NativeAuditDiagnostic", TraceLoggingString(text.c_str(), "ValidatedControl"));
    }
}

bool MachineAuditEnabled(std::optional<RegistryPolicyValue> value) noexcept
{
    // Missing or malformed values cannot become a user-controlled/default-off bypass.
    return !value || value->type != REG_DWORD || value->value != 0;
}

bool IsLegacyAuditPlugin(std::wstring_view name) noexcept
{
    return name.size() == std::size(c_legacyPluginName) - 1 &&
           CompareStringOrdinal(name.data(), static_cast<int>(name.size()), c_legacyPluginName, -1, TRUE) == CSTR_EQUAL;
}

bool QueryMachineAuditEnabled()
{
    RegistryPolicyValue value{};
    DWORD size = sizeof(value.value);
    const auto result = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows\\WSL",
        L"SecurityAuditEnabled",
        RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY,
        &value.type,
        &value.value,
        &size);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    {
        return true;
    }
    if (result != ERROR_SUCCESS || value.type != REG_DWORD || size != sizeof(value.value) || value.value > 1)
    {
        ReportAuditStatus("invalid-machine-policy-default-enabled", HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        return true;
    }
    return MachineAuditEnabled(value);
}

struct NativeAudit::State
{
    State(HostIdentity host, AuditPolicy configuration) : identity(std::move(host)), policy(std::move(configuration))
    {
    }

    const HostIdentity identity;
    const AuditPolicy policy;
    UniqueSocket listener;
    DWORD bootTimeoutMs{};
    wil::unique_event stop{wil::EventOptions::ManualReset};
    std::atomic<bool> started{};
    std::mutex lock;
    std::map<std::string, DistributionIdentity> distributions;
    std::map<std::string, ContainerIdentity> containers;
    std::map<std::string, ContainerIdentity> pendingContainers;
};

NativeAudit::NativeAudit(HostIdentity identity) noexcept
{
    try
    {
        auto self = wil::run_as_self();
        const auto policy = QueryMachineAuditEnabled() ? QueryPolicy() : AuditPolicy{};
        if (!policy.AnyEnabled())
        {
            ReportAuditStatus("disabled-for-vm", S_OK);
            return;
        }
        THROW_HR_IF(E_INVALIDARG, identity.sid.empty());
        m_state = std::make_shared<State>(std::move(identity), policy);
        ReportAuditStatus("configured-for-vm", S_OK);
    }
    catch (...)
    {
        ReportAuditStatus("configure-vm", CurrentAuditFailure());
    }
}

NativeAudit::~NativeAudit()
{
    Stop();
}

void NativeAudit::Start(UniqueSocket listener, DWORD bootTimeoutMs) noexcept
try
{
    if (!m_state)
    {
        return;
    }
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_state->started.exchange(true));
    THROW_HR_IF(E_HANDLE, !listener);
    m_state->listener = std::move(listener);
    m_state->bootTimeoutMs = bootTimeoutMs;
    auto argument = std::make_unique<std::shared_ptr<State>>(m_state);
    wil::unique_handle thread(CreateThread(nullptr, 0, Run, argument.get(), 0, nullptr));
    THROW_LAST_ERROR_IF(!thread);
    argument.release();
}
catch (...)
{
    ReportAuditStatus("start-worker", CurrentAuditFailure());
}

void NativeAudit::Stop() noexcept
{
    if (m_state)
    {
        m_state->stop.SetEvent();
    }
}

void NativeAudit::DistributionStarted(DistributionIdentity identity) noexcept
try
{
    if (m_state)
    {
        std::lock_guard lock(m_state->lock);
        m_state->distributions[identity.guid] = std::move(identity);
    }
}
CATCH_LOG();

void NativeAudit::DistributionStopping(std::string_view guid) noexcept
try
{
    if (m_state)
    {
        std::lock_guard lock(m_state->lock);
        m_state->distributions.erase(std::string(guid));
    }
}
CATCH_LOG();

void NativeAudit::ContainerStarted(ContainerIdentity identity) noexcept
try
{
    if (m_state)
    {
        ValidateContainerInspection(
            {{"version", c_protocolVersion},
             {"kind", "inspect-container"},
             {"containerId", identity.id},
             {"startedAt", identity.startedAt},
             {"pid", identity.initPid}},
            false);
        std::lock_guard lock(m_state->lock);
        m_state->containers[identity.id] = identity;
        m_state->pendingContainers[identity.id] = std::move(identity);
    }
}
catch (...)
{
    ReportAuditStatus("container-attribution-unknown", CurrentAuditFailure());
}

void NativeAudit::ContainerStopping(std::string_view id) noexcept
try
{
    if (m_state)
    {
        std::lock_guard lock(m_state->lock);
        m_state->containers.erase(std::string(id));
        m_state->pendingContainers.erase(std::string(id));
    }
}
CATCH_LOG();

DWORD WINAPI NativeAudit::Run(void* parameter) noexcept
{
    std::unique_ptr<std::shared_ptr<State>> reference(static_cast<std::shared_ptr<State>*>(parameter));
    const auto state = *reference;
    try
    {
        auto self = wil::run_as_self();
        WSADATA data{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
        const auto winsock = wil::scope_exit([] { WSACleanup(); });
        const auto closeListener = wil::scope_exit([&] { state->listener.reset(); });
        auto socket = wsl::windows::common::socket::CancellableAccept(state->listener.get(), state->bootTimeoutMs, state->stop.get());
        state->listener.reset();
        if (socket)
        {
            Collect(state, std::move(*socket));
        }
    }
    catch (...)
    {
        if (!state->stop.is_signaled())
        {
            ReportAuditStatus("collector-failed-until-next-vm", CurrentAuditFailure());
        }
    }
    return 0;
}

void NativeAudit::Collect(const std::shared_ptr<State>& state, UniqueSocket socket)
{
    const auto shutdown = wil::scope_exit([&] { ::shutdown(socket.get(), SD_BOTH); });
    if (state->stop.is_signaled())
    {
        return;
    }
    const auto& policy = state->policy;
    auto output = std::make_shared<WindowsSecuritySink>(policy);
    EventSession events(policy, state->identity, output);
    ConfigureSocket(socket.get());
    SendControl(socket.get(), Configuration(policy));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_auditStartupTimeoutMs);
    NdjsonReceiver receiver;
    const auto acceptBytes = [&](std::string_view bytes) {
        receiver.Feed(bytes, [&](nlohmann::json message) {
            if (message.at("kind") == "container-context")
            {
                THROW_HR_IF(E_INVALIDARG, !events.Ready() || events.Stopped());
                ApplyContainerInspection(*state, message);
                return;
            }
            std::vector<DistributionIdentity> distributions;
            std::vector<ContainerIdentity> containers;
            {
                std::lock_guard lock(state->lock);
                for (const auto& [_, identity] : state->distributions)
                {
                    distributions.emplace_back(identity);
                }
                for (const auto& [_, identity] : state->containers)
                {
                    containers.emplace_back(identity);
                }
            }
            events.SetDistributions(std::move(distributions));
            events.SetContainers(std::move(containers));
            const bool ready = message.at("kind") == "ready";
            events.Accept(std::move(message));
            if (ready)
            {
                ReportAuditStatus("startup-ready", S_OK);
            }
        });
    };
    while (!state->stop.is_signaled())
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !events.Ready() && std::chrono::steady_clock::now() >= deadline);
        if (!Read(socket.get(), acceptBytes) || events.Stopped())
        {
            receiver.Finish();
            events.Finish();
            THROW_HR(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED));
        }
        std::optional<ContainerIdentity> pending;
        {
            std::lock_guard lock(state->lock);
            if (events.Ready() && !state->pendingContainers.empty())
            {
                pending = std::move(state->pendingContainers.begin()->second);
                state->pendingContainers.erase(state->pendingContainers.begin());
            }
        }
        if (pending)
        {
            SendControl(
                socket.get(),
                {{"version", c_protocolVersion},
                 {"kind", "inspect-container"},
                 {"containerId", pending->id},
                 {"startedAt", pending->startedAt},
                 {"pid", pending->initPid}});
        }
    }
    try
    {
        SendControl(socket.get(), {{"version", c_protocolVersion}, {"kind", "stop"}});
        const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!events.Stopped() && std::chrono::steady_clock::now() < stopDeadline)
        {
            if (!Read(socket.get(), acceptBytes))
            {
                break;
            }
        }
        receiver.Finish();
        events.Finish();
        ReportAuditStatus("vm-stopped", S_OK);
    }
    catch (...)
    {
        ReportAuditStatus("stopped-frame-unavailable", CurrentAuditFailure());
    }
}

void NativeAudit::ApplyContainerInspection(State& state, const nlohmann::json& message)
{
    ValidateContainerInspection(message, true);
    std::lock_guard lock(state.lock);
    const auto found = state.containers.find(message.at("containerId").get_ref<const std::string&>());
    if (found == state.containers.end() || found->second.initPid != message.at("pid").get<uint32_t>() ||
        found->second.startedAt != message.at("startedAt").get_ref<const std::string&>())
    {
        return;
    }
    if (!message.at("found").get<bool>())
    {
        ReportAuditStatus("container-attribution-unknown", HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        return;
    }
    found->second.pidNamespace = message.at("pidNamespace").get<uint64_t>();
    found->second.mountNamespace = message.at("mountNamespace").get<uint64_t>();
    found->second.observedAfterNs = message.at("observedAfterNs").get<uint64_t>();
}

} // namespace wsl::audit
