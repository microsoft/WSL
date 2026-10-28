// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginRuntime.h"
#include "Localization.h"

#include <atomic>
#include <objbase.h>

namespace wsl::audit {
namespace {

    void ReleaseWinsock(bool)
    {
        if (WSACleanup() == SOCKET_ERROR)
        {
            LOG_IF_FAILED(HRESULT_FROM_WIN32(WSAGetLastError()));
        }
    }

    using UniqueWinsock = wil::unique_any<bool, decltype(&ReleaseWinsock), ReleaseWinsock>;

    void ConfigureSocket(SOCKET socket)
    {
        const DWORD timeout = 2000;
        const auto result = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result == SOCKET_ERROR);
    }

    std::string GuidString(const GUID& id)
    {
        wchar_t value[40]{};
        THROW_HR_IF(E_INVALIDARG, StringFromGUID2(id, value, static_cast<int>(std::size(value))) == 0);
        return ToUtf8(value);
    }

} // namespace

struct PluginRuntime::SessionContext
{
    SessionKey key;
    AuditPolicy policy;
    std::shared_ptr<AuditSink> output;
    std::unique_ptr<EventSession> events;
    NdjsonReceiver receiver;
    UniqueWinsock winsock;
    UniqueSocket socket;
    std::unique_ptr<WslcProcess> process;
    wil::unique_handle thread;
    std::atomic<HRESULT> result{S_OK};
    std::mutex stopLock;
    bool stopping{};
    bool mounted{};
    bool stderrOpen{true};
    std::vector<DistributionIdentity> distributions;
    std::vector<ContainerIdentity> containers;

    SOCKET Input() const
    {
        return process ? process->Input() : socket.get();
    }
    SOCKET Output() const
    {
        return process ? process->Output() : socket.get();
    }

    void Shutdown() const noexcept
    {
        if (process)
        {
            process->Shutdown();
        }
        else if (socket && shutdown(socket.get(), SD_BOTH) == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            if (error != WSAENOTCONN)
            {
                LOG_IF_FAILED(HRESULT_FROM_WIN32(error));
            }
        }
    }
};

PluginRuntime::PluginRuntime(const WSLPluginAPIV1& api, PluginServices services) :
    m_mount(api.MountFolder), m_execute(api.ExecuteBinary), m_pluginError(api.PluginError), m_wslc(GetWslcApi(api)), m_services(std::move(services))
{
    THROW_HR_IF(
        E_INVALIDARG,
        !m_mount || !m_execute || !m_pluginError || !m_services.queryPolicy || !m_services.payloadFolder || !m_services.createSink);
}

std::shared_ptr<PluginRuntime::SessionContext> PluginRuntime::GetContext(SessionKey key)
{
    std::lock_guard lock(m_lock);
    const auto found = m_sessions.find(key);
    return found == m_sessions.end() ? nullptr : found->second;
}

bool PluginRuntime::Receive(SessionContext& context)
{
    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(context.Output(), &readable);
    if (context.process && context.stderrOpen)
    {
        FD_SET(context.process->Error(), &readable);
    }
    timeval timeout{0, 200000};
    const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
    if (ready == 0)
    {
        return true;
    }
    std::array<char, 8192> bytes{};
    if (context.process && context.stderrOpen && FD_ISSET(context.process->Error(), &readable))
    {
        const auto count = recv(context.process->Error(), bytes.data(), static_cast<int>(bytes.size()), 0);
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
        context.stderrOpen = count != 0;
        if (count > 0)
        {
            // Drain independently of stdout; do not copy arbitrary guest diagnostic text into host logs.
            OutputDebugStringW(L"WSL Security audit: WSLC collector emitted stderr diagnostics.\n");
        }
    }
    if (!FD_ISSET(context.Output(), &readable))
    {
        return true;
    }
    const auto count = recv(context.Output(), bytes.data(), static_cast<int>(bytes.size()), 0);
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
    if (count == 0)
    {
        context.receiver.Finish();
        context.events->Finish();
        return false;
    }
    context.receiver.Feed(
        std::string_view(bytes.data(), count), [&](nlohmann::json message) { context.events->Accept(std::move(message)); });
    return !context.events->Stopped();
}

DWORD WINAPI PluginRuntime::ReceiverThread(void* argument) noexcept
{
    std::unique_ptr<std::shared_ptr<SessionContext>> reference(static_cast<std::shared_ptr<SessionContext>*>(argument));
    const auto context = *reference;
    try
    {
        while (Receive(*context))
        {
        }
        context->receiver.Finish();
        context->events->Finish();
        context->output->Flush();
    }
    catch (...)
    {
        context->result = wil::ResultFromCaughtException();
        LOG_IF_FAILED_MSG(context->result.load(), "Security collector transport, protocol or output failed.");
        try
        {
            context->output->Write(
                {{"version", c_protocolVersion},
                 {"kind", "error"},
                 {"source", context->key.first ? "wslc-plugin" : "windows-plugin"},
                 {"message", "Collector transport/protocol/output failed; no exception input bytes are logged"},
                 {"hresult", static_cast<ULONG>(context->result.load())},
                 {"receivedUtc", UtcNow()}});
        }
        CATCH_LOG();
        context->Shutdown();
    }
    return 0;
}

void PluginRuntime::Start(SessionKey key, HostIdentity host)
{
    auto context = std::make_shared<SessionContext>();
    context->key = key;
    context->policy = m_services.queryPolicy();
    context->output = m_services.createSink(context->policy);
    context->events = std::make_unique<EventSession>(context->policy, std::move(host), context->output);
    std::unique_lock startLock(context->stopLock);
    {
        std::lock_guard lock(m_lock);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_sessions.contains(key));
        m_sessions.emplace(key, context);
    }
    auto cleanup = wil::scope_exit([&] {
        context->stopping = true;
        startLock.unlock();
        {
            std::lock_guard lock(m_lock);
            const auto found = m_sessions.find(key);
            if (found != m_sessions.end() && found->second == context)
            {
                m_sessions.erase(found);
            }
        }
        try
        {
            StopContext(context);
        }
        CATCH_LOG();
    });
    if (!context->policy.AnyEnabled())
    {
        cleanup.release();
        return;
    }
    const auto folder = m_services.payloadFolder();
    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    context->winsock.reset(true);
    auto arguments = CollectorArguments("--package", "/wsl-security-audit");
    if (key.first)
    {
        THROW_HR_IF(E_NOINTERFACE, !m_wslc);
        THROW_IF_FAILED(m_wslc->mount(key.second, folder.c_str(), "/wsl-security-audit", TRUE));
        context->mounted = true;
        context->process = std::make_unique<WslcProcess>(*m_wslc);
        context->process->Start(key.second, std::move(arguments));
    }
    else
    {
        THROW_IF_FAILED(m_mount(key.second, folder.c_str(), L"/wsl-security-audit", TRUE, L"wsl-security-audit"));
        SOCKET socket = INVALID_SOCKET;
        const auto result = m_execute(key.second, arguments[0], arguments.data(), &socket);
        context->socket.reset(socket);
        THROW_IF_FAILED(result);
        THROW_HR_IF(E_HANDLE, !context->socket);
    }
    ConfigureSocket(context->Input());
    SendAll(context->Input(), EncodeMessage(Configuration(context->policy)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!context->events->Ready())
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::chrono::steady_clock::now() >= deadline);
        THROW_HR_IF(E_UNEXPECTED, !Receive(*context));
    }
    auto reference = std::make_unique<std::shared_ptr<SessionContext>>(context);
    context->thread.reset(CreateThread(nullptr, 0, ReceiverThread, reference.get(), 0, nullptr));
    THROW_LAST_ERROR_IF(!context->thread);
    reference.release();
    cleanup.release();
}

void PluginRuntime::StopContext(const std::shared_ptr<SessionContext>& context)
{
    std::lock_guard lock(context->stopLock);
    context->stopping = true;
    HRESULT result = S_OK;
    if (context->thread)
    {
        if (WaitForSingleObject(context->thread.get(), 0) == WAIT_TIMEOUT)
        {
            result = wil::ResultFromException(
                [&] { SendAll(context->Input(), EncodeMessage({{"version", c_protocolVersion}, {"kind", "stop"}})); });
            auto wait = WaitForSingleObject(context->thread.get(), 3000);
            THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
            if (wait == WAIT_TIMEOUT)
            {
                context->Shutdown();
                wait = WaitForSingleObject(context->thread.get(), 2000);
                THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
                // The worker retains the context if teardown exceeds its bound.
                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), wait != WAIT_OBJECT_0);
                result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
        context->thread.reset();
    }
    else
    {
        context->Shutdown();
    }
    if (context->process)
    {
        const auto exitResult = wil::ResultFromException(
            [&] { THROW_HR_IF_MSG(E_FAIL, context->process->WaitForExit(1000) != 0, "WSLC collector exited unsuccessfully."); });
        LOG_IF_FAILED(exitResult);
        if (SUCCEEDED(result))
        {
            result = exitResult;
        }
        context->process->Shutdown();
        context->process.reset();
    }
    context->socket.reset();
    if (context->mounted)
    {
        LOG_IF_FAILED(m_wslc->unmount(context->key.second, "/wsl-security-audit"));
        context->mounted = false;
    }
    THROW_IF_FAILED(result);
    THROW_IF_FAILED(context->result.load());
}

void PluginRuntime::Stop(SessionKey key)
{
    std::shared_ptr<SessionContext> context;
    {
        std::lock_guard lock(m_lock);
        const auto found = m_sessions.find(key);
        if (found == m_sessions.end())
        {
            return;
        }
        context = found->second;
        m_sessions.erase(found);
    }
    StopContext(context);
}

HRESULT PluginRuntime::FailStartup(bool wslc) const noexcept
{
    const auto error = wil::ResultFromCaughtException();
    LOG_IF_FAILED_MSG(error, "Security audit startup failed (%hs).", wslc ? "WSLC" : "WSL");
    if (!wslc)
    {
        LOG_IF_FAILED(m_pluginError(wsl::shared::Localization::MessageSecurityAuditStartFailed().c_str()));
    }
    return error;
}

HRESULT PluginRuntime::VmStarted(const WSLSessionInformation* session) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session);
        Start({false, session->SessionId}, {SidString(session->UserSid), session->SessionId, QueryTokenSessionId(session->UserToken)});
        return S_OK;
    }
    catch (...)
    {
        return FailStartup(false);
    }
}

HRESULT PluginRuntime::VmStopping(const WSLSessionInformation* session) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session);
        Stop({false, session->SessionId});
        return S_OK;
    }
    CATCH_RETURN();
}

HRESULT PluginRuntime::DistributionStarted(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session || !distribution || !distribution->Name);
        const auto context = GetContext({false, session->SessionId});
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !context);
        std::lock_guard lock(context->stopLock);
        THROW_HR_IF(E_ABORT, context->stopping);
        THROW_IF_FAILED(context->result.load());
        DistributionIdentity identity{ToUtf8(distribution->Name), GuidString(distribution->Id), distribution->PidNamespace};
        std::erase_if(context->distributions, [&](const auto& item) { return item.guid == identity.guid; });
        context->distributions.push_back(std::move(identity));
        context->events->SetDistributions(context->distributions);
        return S_OK;
    }
    catch (...)
    {
        return FailStartup(false);
    }
}

HRESULT PluginRuntime::DistributionStopping(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session || !distribution);
        if (const auto context = GetContext({false, session->SessionId}))
        {
            std::lock_guard lock(context->stopLock);
            const auto guid = GuidString(distribution->Id);
            std::erase_if(context->distributions, [&](const auto& item) { return item.guid == guid; });
            context->events->SetDistributions(context->distributions);
            THROW_IF_FAILED(context->result.load());
        }
        return S_OK;
    }
    CATCH_RETURN();
}

HRESULT PluginRuntime::WslcVmStarted(const WSLCSessionInformation* session) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session || !session->DisplayName || !m_wslc);
        const auto name = ToUtf8(session->DisplayName);
        THROW_HR_IF(E_INVALIDARG, name.empty() || name.size() > c_maxWslcSessionNameBytes);
        Start({true, session->SessionId}, {SidString(session->UserSid), {}, QueryTokenSessionId(session->UserToken), session->SessionId, name});
        return S_OK;
    }
    catch (...)
    {
        return FailStartup(true);
    }
}

HRESULT PluginRuntime::WslcVmStopping(const WSLCSessionInformation* session) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session);
        Stop({true, session->SessionId});
        return S_OK;
    }
    CATCH_RETURN();
}

HRESULT PluginRuntime::ContainerStarted(const WSLCSessionInformation* session, LPCSTR inspect) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session || !inspect || !m_wslc);
        const auto context = GetContext({true, session->SessionId});
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_READY), !context, "WSLC Security collector did not start.");
        std::lock_guard lock(context->stopLock);
        THROW_HR_IF(E_ABORT, context->stopping);
        if (!context->policy.AnyEnabled())
        {
            return S_OK;
        }
        THROW_IF_FAILED(context->result.load());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_READY), !context->events->Ready() || context->events->Stopped());
        const auto length = strnlen_s(inspect, 1024 * 1024 + 1);
        auto identity = ReadContainerIdentity(std::string_view(inspect, length));
        std::erase_if(context->containers, [&](const auto& item) { return item.id == identity.id; });
        context->events->SetContainers(context->containers);
        if (identity.initPid != 0)
        {
            const auto result = wil::ResultFromException([&] { ResolveContainerNamespaces(*m_wslc, session->SessionId, identity); });
            LOG_IF_FAILED_MSG(result, "WSLC namespace lookup failed; collection continues with unknown container attribution.");
        }
        else
        {
            LOG_HR_MSG(
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "WSLC container already exited; its namespace attribution is unavailable.");
        }
        context->containers.push_back(std::move(identity));
        context->events->SetContainers(context->containers);
        return S_OK;
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        LOG_IF_FAILED_MSG(error, "Cannot establish WSLC container audit attribution; rejecting container start.");
        return error;
    }
}

HRESULT PluginRuntime::ContainerStopping(const WSLCSessionInformation* session, LPCSTR containerId) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !session || !containerId || strnlen_s(containerId, 129) > 128);
        if (const auto context = GetContext({true, session->SessionId}))
        {
            std::lock_guard lock(context->stopLock);
            std::erase_if(context->containers, [&](const auto& item) { return item.id == containerId; });
            context->events->SetContainers(context->containers);
        }
        return S_OK;
    }
    CATCH_RETURN();
}

} // namespace wsl::audit
