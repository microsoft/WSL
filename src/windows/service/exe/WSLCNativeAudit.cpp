// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCPluginNotifier.h"

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::service::wslc::WSLCPluginNotifier;

namespace {
struct AuditFactory
{
    wil::com_ptr<IStream> marshaled;
    wil::com_ptr<IWSLCNativeAuditProcessFactory> factory;
    ULONGLONG generation{};

    ~AuditFactory()
    {
        if (marshaled)
        {
            LOG_IF_FAILED(CoReleaseMarshalData(marshaled.get()));
        }
    }

    wsl::audit::AuditTransport Create(const std::vector<std::string>& arguments)
    {
        if (!factory)
        {
            THROW_IF_FAILED(CoGetInterfaceAndReleaseStream(marshaled.detach(), __uuidof(IWSLCNativeAuditProcessFactory), factory.put_void()));
        }
        ULONG pid{};
        LPCSTR container = nullptr;
        if (arguments.size() == 5 && arguments[1] == "--inspect-pid" && arguments[3] == "--container-id")
        {
            pid = static_cast<ULONG>(std::stoul(arguments[2]));
            container = arguments[4].c_str();
        }
        else
        {
            THROW_HR_IF(
                E_INVALIDARG, arguments != std::vector<std::string>({"/wsl-audit-collector", "--package", "/wsl-security-audit"}));
        }

        // Bound a stalled COM call independently; lifecycle callbacks never join this worker.
        struct Deadline
        {
            DWORD thread = GetCurrentThreadId();
            wil::unique_event complete{wil::EventOptions::ManualReset};
            std::mutex lock;
            bool active{true};
        };
        auto deadline = std::make_shared<Deadline>();
        THROW_IF_FAILED(CoEnableCallCancellation(nullptr));
        auto disable = wil::scope_exit([] { CoDisableCallCancellation(nullptr); });
        auto argument = std::make_unique<std::shared_ptr<Deadline>>(deadline);
        THROW_IF_WIN32_BOOL_FALSE(TrySubmitThreadpoolCallback(
            [](PTP_CALLBACK_INSTANCE, void* raw) {
                std::unique_ptr<std::shared_ptr<Deadline>> state(static_cast<std::shared_ptr<Deadline>*>(raw));
                if (!(*state)->complete.wait(wsl::audit::c_auditProcessTimeoutMs))
                {
                    std::lock_guard lock((*state)->lock);
                    if ((*state)->active)
                    {
                        LOG_IF_FAILED(CoCancelCall((*state)->thread, 0));
                    }
                }
            },
            argument.get(),
            nullptr));
        argument.release();
        auto complete = wil::scope_exit([&] {
            std::lock_guard lock(deadline->lock);
            deadline->active = false;
            deadline->complete.SetEvent();
        });
        WSLCHandle input{}, output{}, error{};
        const auto result = factory->CreateAuditProcess(generation, pid, container, &input, &output, &error);
        wsl::audit::AuditTransport transport;
        const auto take = [](const WSLCHandle& handle) {
            THROW_HR_IF(E_UNEXPECTED, handle.Type != WSLCHandleTypeSocket && handle.Type != WSLCHandleTypeUnknown);
            return wsl::audit::UniqueSocket(handle.Type == WSLCHandleTypeSocket ? reinterpret_cast<SOCKET>(handle.Handle.Socket) : INVALID_SOCKET);
        };
        transport.input = take(input);
        transport.output = take(output);
        transport.error = take(error);
        THROW_IF_FAILED(result);
        THROW_HR_IF(E_HANDLE, !transport.input || !transport.output);
        return transport;
    }
};
} // namespace

HRESULT WSLCPluginNotifier::OnNativeAuditStarted(ULONGLONG Generation, IWSLCNativeAuditProcessFactory* Factory)
try
{
    COMServiceExecutionContext context;
    RETURN_HR_IF(E_INVALIDARG, Generation == 0 || !Factory);
    auto factory = std::make_shared<AuditFactory>();
    factory->generation = Generation;
    THROW_IF_FAILED(CoMarshalInterThreadInterfaceInStream(__uuidof(IWSLCNativeAuditProcessFactory), Factory, &factory->marshaled));
    wsl::audit::HostIdentity identity;
    identity.sid = wsl::audit::SidString(m_sessionInfo.UserSid);
    identity.windowsSessionId = wsl::audit::QueryTokenSessionId(m_userToken.get());
    identity.wslcSessionId = m_sessionInfo.SessionId;
    identity.wslcSessionName = wsl::audit::ToUtf8(m_displayName);
    auto lock = m_auditLock.lock_exclusive();
    m_auditGeneration = Generation;
    m_securityAudit.Start(std::move(identity), [factory](const auto& arguments) { return factory->Create(arguments); });
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnNativeAuditStopping(ULONGLONG Generation)
try
{
    COMServiceExecutionContext context;
    auto lock = m_auditLock.lock_exclusive();
    if (Generation == m_auditGeneration)
    {
        m_securityAudit.Stop();
        m_auditGeneration = 0;
    }
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::WaitForNativeAuditStartup(ULONGLONG Generation)
try
{
    COMServiceExecutionContext context;
    wsl::audit::NativeAudit::StartupWait startup;
    {
        auto lock = m_auditLock.lock_shared();
        if (Generation == 0 || Generation != m_auditGeneration)
        {
            return S_OK;
        }
        startup = m_securityAudit.CaptureStartupWait();
    }
    // Stop and a replacement generation must remain able to enter while this epoch waits.
    (void)startup.Wait();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnNativeAuditContainerStarted(ULONGLONG Generation, const WSLCNativeAuditContainer* Container)
try
{
    COMServiceExecutionContext context;
    RETURN_HR_IF(E_POINTER, !Container);
    const auto text = [](const auto& value) {
        const auto length = strnlen(value, std::size(value));
        THROW_HR_IF(E_INVALIDARG, length == 0 || length == std::size(value));
        return std::string(value, length);
    };
    wsl::audit::ContainerIdentity identity;
    identity.id = text(Container->Id);
    auto lock = m_auditLock.lock_shared();
    if (Generation == m_auditGeneration)
    {
        m_securityAudit.ContainerStopping(identity.id);
        identity.name = text(Container->Name);
        identity.startedAt = text(Container->StartedAt);
        identity.initPid = Container->InitPid;
        m_securityAudit.ContainerStarted(std::move(identity));
    }
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnNativeAuditContainerStopping(ULONGLONG Generation, LPCSTR ContainerId)
try
{
    COMServiceExecutionContext context;
    RETURN_HR_IF(E_POINTER, !ContainerId);
    auto lock = m_auditLock.lock_shared();
    if (Generation == m_auditGeneration)
    {
        m_securityAudit.ContainerStopping(ContainerId);
    }
    return S_OK;
}
CATCH_RETURN();
