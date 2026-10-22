// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "NativeAudit.h"
#include "WindowsSecuritySink.h"
#include "WslTelemetry.h"
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
            // Guest protocol/JSON exceptions must not trigger WIL's debug fail-fast policy
            // or expose their potentially sensitive input in diagnostic exception strings.
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_auditProcessTimeoutMs);
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

    bool Read(AuditTransport& transport, bool& stderrOpen, const std::function<void(std::string_view)>& consume)
    {
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(transport.output.get(), &readable);
        if (stderrOpen && transport.error)
        {
            FD_SET(transport.error.get(), &readable);
        }
        timeval timeout{0, 200000};
        const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
        std::array<char, 8192> bytes{};
        if (stderrOpen && transport.error && FD_ISSET(transport.error.get(), &readable))
        {
            const auto count = recv(transport.error.get(), bytes.data(), static_cast<int>(bytes.size()), 0);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
            stderrOpen = count != 0;
            if (count > 0)
            {
                // Guest stderr may include arguments or paths: record only its presence.
                ReportAuditStatus("collector-stderr", S_OK);
            }
        }
        if (FD_ISSET(transport.output.get(), &readable))
        {
            const auto count = recv(transport.output.get(), bytes.data(), static_cast<int>(bytes.size()), 0);
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

void ReportAuditStatus(PCSTR stage, HRESULT result, unsigned attempt) noexcept
{
    if (g_hTraceLoggingProvider)
    {
        WSL_LOG(
            "NativeAuditStatus",
            TraceLoggingString(stage, "Stage"),
            TraceLoggingHResult(result, "Result"),
            TraceLoggingUInt32(attempt, "Attempt"));
    }
    LOG_IF_FAILED_MSG(result, "Native WSL Security audit: %hs (attempt %u). Workload startup remains available.", stage, attempt);
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

void AuditTransport::Shutdown() const noexcept
{
    for (const auto socket : {input.get(), output.get(), error.get()})
    {
        if (socket != INVALID_SOCKET)
        {
            shutdown(socket, SD_BOTH);
        }
    }
}

struct NativeAudit::State
{
    using Clock = std::chrono::steady_clock;

    explicit State(const NativeAuditServices& source) :
        services(source),
        startupDeadline(Clock::now() + std::chrono::milliseconds(std::min(source.startupTimeoutMs, c_auditStartupTimeoutMs)))
    {
    }

    void CompleteStartup(StartupResult result) noexcept
    {
        auto expected = StartupResult::Pending;
        if (!startupResult.compare_exchange_strong(expected, result))
        {
            return;
        }
        startupComplete.SetEvent();
        switch (result)
        {
        case StartupResult::Ready:
            ReportAuditStatus("startup-ready", S_OK);
            break;
        case StartupResult::Disabled:
            ReportAuditStatus("startup-disabled", S_OK);
            break;
        case StartupResult::TimedOut:
            ReportAuditStatus("startup-timeout-workload-allowed", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            break;
        case StartupResult::Cancelled:
            ReportAuditStatus("startup-cancelled", HRESULT_FROM_WIN32(ERROR_CANCELLED));
            break;
        default:
            ReportAuditStatus("startup-failed-workload-allowed", E_FAIL);
            break;
        }
    }

    bool StartupExpired() const noexcept
    {
        return startupResult.load() != StartupResult::Ready && Clock::now() >= startupDeadline;
    }

    HostIdentity identity;
    NativeAuditServices services;
    wil::unique_event stop{wil::EventOptions::ManualReset};
    wil::unique_event startupComplete{wil::EventOptions::ManualReset};
    const Clock::time_point startupDeadline;
    std::atomic<StartupResult> startupResult{StartupResult::Pending};
    std::mutex lock;
    std::map<std::string, DistributionIdentity> distributions;
    std::map<std::string, ContainerIdentity> containers;
    std::map<std::string, ContainerIdentity> pendingContainers;
    bool inspectionRunning{};
};

struct NativeAudit::Work
{
    std::shared_ptr<State> state;
    AuditProcessFactory factory;
};

NativeAudit::NativeAudit(NativeAuditServices services) : m_services(std::move(services))
{
    if (!m_services.sink)
    {
        m_services.sink = [](const AuditPolicy& policy) { return std::make_shared<WindowsSecuritySink>(policy); };
    }
}

NativeAudit::~NativeAudit()
{
    Stop();
}

void NativeAudit::Start(HostIdentity identity, AuditProcessFactory factory) noexcept
try
{
    Stop();
    m_worker.reset();
    auto state = std::make_shared<State>(m_services);
    state->identity = std::move(identity);
    auto argument = std::make_unique<Work>(Work{state, std::move(factory)});
    m_state.store(state);
    wil::unique_handle thread(CreateThread(nullptr, 0, Run, argument.get(), 0, nullptr));
    THROW_LAST_ERROR_IF(!thread);
    argument.release();
    m_worker = std::move(thread);
}
catch (...)
{
    Stop();
    ReportAuditStatus("start-worker", CurrentAuditFailure());
}

void NativeAudit::Stop() noexcept
{
    if (const auto state = m_state.exchange(nullptr))
    {
        state->stop.SetEvent();
        state->CompleteStartup(StartupResult::Cancelled);
    }
}

NativeAudit::StartupWait NativeAudit::CaptureStartupWait() const noexcept
{
    return StartupWait{m_state.load()};
}

NativeAudit::StartupResult NativeAudit::StartupWait::Wait() const noexcept
{
    if (!m_state)
    {
        ReportAuditStatus("startup-unavailable-workload-allowed", E_FAIL);
        return StartupResult::Failed;
    }
    const HANDLE handles[]{m_state->stop.get(), m_state->startupComplete.get()};
    for (;;)
    {
        if (m_state->stop.is_signaled())
        {
            return StartupResult::Cancelled;
        }
        const auto result = m_state->startupResult.load();
        if (result != StartupResult::Pending)
        {
            return result;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(m_state->startupDeadline - State::Clock::now()).count();
        if (remaining <= 0)
        {
            m_state->CompleteStartup(StartupResult::TimedOut);
            continue;
        }
        const auto wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, static_cast<DWORD>(remaining));
        if (wait == WAIT_FAILED)
        {
            ReportAuditStatus("startup-wait-failed-workload-allowed", HRESULT_FROM_WIN32(GetLastError()));
            return StartupResult::Failed;
        }
    }
}

void NativeAudit::DistributionStarted(DistributionIdentity identity) noexcept
try
{
    if (const auto state = m_state.load())
    {
        std::lock_guard lock(state->lock);
        state->distributions[identity.guid] = std::move(identity);
    }
}
CATCH_LOG();

void NativeAudit::DistributionStopping(std::string_view guid) noexcept
try
{
    if (const auto state = m_state.load())
    {
        std::lock_guard lock(state->lock);
        state->distributions.erase(std::string(guid));
    }
}
CATCH_LOG();

void NativeAudit::ContainerStarted(ContainerIdentity identity) noexcept
try
{
    if (const auto state = m_state.load())
    {
        std::lock_guard lock(state->lock);
        state->containers[identity.id] = identity;
        state->pendingContainers[identity.id] = std::move(identity);
    }
}
CATCH_LOG();

void NativeAudit::ContainerStopping(std::string_view id) noexcept
try
{
    if (const auto state = m_state.load())
    {
        std::lock_guard lock(state->lock);
        state->containers.erase(std::string(id));
        state->pendingContainers.erase(std::string(id));
    }
}
CATCH_LOG();

DWORD WINAPI NativeAudit::Run(void* parameter) noexcept
{
    std::unique_ptr<Work> reference(static_cast<Work*>(parameter));
    const auto state = reference->state;
    const auto startupFinished = wil::scope_exit(
        [&] { state->CompleteStartup(state->stop.is_signaled() ? StartupResult::Cancelled : StartupResult::Failed); });
    try
    {
        auto self = wil::run_as_self();
        THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const auto uninitialize = wil::scope_exit([] { CoUninitialize(); });
        // In WSLC this owns a COM factory. Release it on this worker, before COM uninitializes,
        // even if policy disables collection; never leave proxy cleanup to Stop under a VM lock.
        const auto releaseFactory = wil::scope_exit([&] { reference->factory = {}; });
        WSADATA data{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
        const auto winsock = wil::scope_exit([] { WSACleanup(); });
        for (unsigned attempt = 1; attempt <= c_maxCollectorAttempts && !state->stop.is_signaled(); ++attempt)
        {
            if (state->StartupExpired())
            {
                state->CompleteStartup(StartupResult::TimedOut);
                return 0;
            }
            try
            {
                Collect(state, reference->factory);
                return 0;
            }
            catch (...)
            {
                if (state->stop.is_signaled())
                {
                    return 0;
                }
                ReportAuditStatus("collector-unavailable", CurrentAuditFailure(), attempt);
                DWORD retryDelay = attempt * 1000;
                if (state->startupResult.load() != StartupResult::Ready)
                {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(state->startupDeadline - State::Clock::now()).count();
                    if (remaining <= 0)
                    {
                        state->CompleteStartup(StartupResult::TimedOut);
                        return 0;
                    }
                    retryDelay = std::min(retryDelay, static_cast<DWORD>(remaining));
                }
                if (attempt < c_maxCollectorAttempts && state->stop.wait(retryDelay))
                {
                    return 0;
                }
            }
        }
        if (!state->stop.is_signaled())
        {
            ReportAuditStatus("retry-budget-exhausted", HRESULT_FROM_WIN32(ERROR_RETRY), c_maxCollectorAttempts);
        }
    }
    catch (...)
    {
        ReportAuditStatus("worker-initialization", CurrentAuditFailure());
    }
    return 0;
}

void NativeAudit::Collect(const std::shared_ptr<State>& state, const AuditProcessFactory& factory)
{
    if (!state->services.enabled())
    {
        state->CompleteStartup(StartupResult::Disabled);
        ReportAuditStatus("administrator-disabled", S_OK);
        return;
    }
    const auto policy = state->services.policy();
    if (!policy.AnyEnabled())
    {
        state->CompleteStartup(StartupResult::Disabled);
        ReportAuditStatus("audit-subcategories-disabled", S_OK);
        return;
    }
    auto output = state->services.sink(policy);
    EventSession events(policy, state->identity, output);
    if (state->stop.is_signaled())
    {
        return;
    }
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), state->StartupExpired());
    auto transport = factory({"/wsl-audit-collector", "--package", "/wsl-security-audit"});
    const auto cleanup = wil::scope_exit([&] { transport.Shutdown(); });
    if (state->stop.is_signaled())
    {
        return;
    }
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), state->StartupExpired());
    THROW_HR_IF(E_HANDLE, !transport.output);
    ConfigureSocket(transport.Input());
    SendControl(transport.Input(), Configuration(policy));
    const auto deadline = state->startupResult.load() == StartupResult::Ready
                              ? State::Clock::now() + std::chrono::milliseconds(c_auditStartupTimeoutMs)
                              : state->startupDeadline;
    NdjsonReceiver receiver;
    bool stderrOpen = true;
    const auto acceptBytes = [&](std::string_view bytes) {
        receiver.Feed(bytes, [&](nlohmann::json message) {
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
            events.Accept(std::move(message));
            if (events.Ready())
            {
                state->CompleteStartup(State::Clock::now() < state->startupDeadline ? StartupResult::Ready : StartupResult::TimedOut);
            }
        });
    };
    while (!state->stop.is_signaled())
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !events.Ready() && std::chrono::steady_clock::now() >= deadline);
        const bool connected = Read(transport, stderrOpen, acceptBytes);
        if (!connected || events.Stopped())
        {
            receiver.Finish();
            events.Finish();
            // STOPPED is valid framing, but unexpected while this VM epoch remains active.
            THROW_HR(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED));
        }
        std::optional<ContainerIdentity> pending;
        {
            std::lock_guard lock(state->lock);
            if (!state->pendingContainers.empty() && !state->inspectionRunning)
            {
                pending = std::move(state->pendingContainers.begin()->second);
                state->pendingContainers.erase(state->pendingContainers.begin());
                state->inspectionRunning = true;
            }
        }
        if (pending)
        {
            try
            {
                // Namespace probes must not stall the collector's bounded event queue.
                // At most one probe is in flight per epoch, without a VM lifetime token.
                struct Probe
                {
                    std::shared_ptr<State> state;
                    ContainerIdentity identity;
                    AuditProcessFactory factory;
                };
                auto probe = std::make_unique<Probe>(Probe{state, std::move(*pending), factory});
                THROW_IF_WIN32_BOOL_FALSE(TrySubmitThreadpoolCallback(
                    [](PTP_CALLBACK_INSTANCE, void* raw) {
                        std::unique_ptr<Probe> probe(static_cast<Probe*>(raw));
                        const auto completed = wil::scope_exit([&] {
                            std::lock_guard lock(probe->state->lock);
                            probe->state->inspectionRunning = false;
                        });
                        try
                        {
                            auto self = wil::run_as_self();
                            THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
                            const auto uninitialize = wil::scope_exit([] { CoUninitialize(); });
                            const auto releaseFactory = wil::scope_exit([&] { probe->factory = {}; });
                            WSADATA data{};
                            THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
                            const auto winsock = wil::scope_exit([] { WSACleanup(); });
                            if (!probe->state->stop.is_signaled())
                            {
                                ResolveContainer(probe->state, std::move(probe->identity), probe->factory);
                            }
                        }
                        catch (...)
                        {
                            ReportAuditStatus("container-attribution-unknown", CurrentAuditFailure());
                        }
                    },
                    probe.get(),
                    nullptr));
                probe.release();
            }
            catch (...)
            {
                {
                    std::lock_guard lock(state->lock);
                    state->inspectionRunning = false;
                }
                ReportAuditStatus("container-attribution-unknown", CurrentAuditFailure());
            }
        }
    }
    // This bounded drain runs only on the worker, never on a VM lifecycle callback.
    try
    {
        SendControl(transport.Input(), {{"version", c_protocolVersion}, {"kind", "stop"}});
        const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!events.Stopped() && std::chrono::steady_clock::now() < stopDeadline)
        {
            if (!Read(transport, stderrOpen, acceptBytes))
            {
                break;
            }
        }
        receiver.Finish();
        events.Finish();
        ReportAuditStatus("vm-epoch-stopped", S_OK);
    }
    catch (...)
    {
        ReportAuditStatus("stopped-frame-unavailable", CurrentAuditFailure());
    }
}

void NativeAudit::ResolveContainer(const std::shared_ptr<State>& state, ContainerIdentity identity, const AuditProcessFactory& factory)
{
    THROW_HR_IF(E_INVALIDARG, identity.initPid == 0);
    auto probe = factory({"/wsl-audit-collector", "--inspect-pid", std::to_string(identity.initPid), "--container-id", identity.id});
    const auto cleanup = wil::scope_exit([&] { probe.Shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string output;
    bool stderrOpen = true;
    while (!state->stop.is_signaled())
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::chrono::steady_clock::now() >= deadline);
        if (!Read(probe, stderrOpen, [&](std::string_view bytes) {
                THROW_HR_IF(E_INVALIDARG, output.size() + bytes.size() > 4096);
                output.append(bytes);
            }))
        {
            break;
        }
    }
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), state->stop.is_signaled());
    const auto document = nlohmann::json::parse(output);
    THROW_HR_IF(
        E_INVALIDARG,
        !document.is_object() || document.size() != 4 || !document.at("pid").is_number_unsigned() ||
            document.at("pid") != identity.initPid);
    const auto readNamespace = [&](const char* name) {
        const auto& value = document.at(name);
        THROW_HR_IF(E_INVALIDARG, !value.is_number_unsigned() || value == 0 || value.get<uint64_t>() > UINT32_MAX);
        return value.get<uint64_t>();
    };
    identity.pidNamespace = readNamespace("pidNamespace");
    identity.mountNamespace = readNamespace("mountNamespace");
    const auto& observed = document.at("observedAfterNs");
    THROW_HR_IF(E_INVALIDARG, !observed.is_number_unsigned() || observed == 0);
    identity.observedAfterNs = observed.get<uint64_t>();
    std::lock_guard lock(state->lock);
    const auto found = state->containers.find(identity.id);
    if (found != state->containers.end() && found->second.initPid == identity.initPid && found->second.startedAt == identity.startedAt)
    {
        found->second = std::move(identity);
    }
}

} // namespace wsl::audit
