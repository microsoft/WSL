// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginRuntime.h"
#include "WindowsSecuritySink.h"

#include <wil/token_helpers.h>

using namespace wsl::audit;

namespace {

std::unique_ptr<PluginRuntime> g_runtime;

HRESULT OnVMStarted(const WSLSessionInformation* session, const WSLVmCreationSettings*) noexcept
{
    return g_runtime->VmStarted(session);
}

HRESULT OnVMStopping(const WSLSessionInformation* session) noexcept
{
    return g_runtime->VmStopping(session);
}

HRESULT OnDistributionStarted(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept
{
    return g_runtime->DistributionStarted(session, distribution);
}

HRESULT OnDistributionStopping(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept
{
    return g_runtime->DistributionStopping(session, distribution);
}

HRESULT OnWslcVmStarted(const WSLCSessionInformation* session) noexcept
{
    return g_runtime->WslcVmStarted(session);
}

HRESULT OnWslcVmStopping(const WSLCSessionInformation* session) noexcept
{
    return g_runtime->WslcVmStopping(session);
}

HRESULT OnContainerStarted(const WSLCSessionInformation* session, LPCSTR inspect) noexcept
{
    return g_runtime->ContainerStarted(session, inspect);
}

HRESULT OnContainerStopping(const WSLCSessionInformation* session, LPCSTR containerId) noexcept
{
    return g_runtime->ContainerStopping(session, containerId);
}

} // namespace

extern "C" __declspec(dllexport) HRESULT WSLPluginAPIV1_EntryPoint(const WSLPluginAPIV1* api, WSLPluginHooksV1* hooks) noexcept
{
    try
    {
        THROW_HR_IF(E_INVALIDARG, !api || !hooks);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), g_runtime != nullptr);
        PluginServices services{
            [] {
                auto authority = security_detail::PrimaryAuthority(SE_SECURITY_NAME, false);
                auto impersonation = wil::impersonate_token(authority.get());
                return QueryPolicy();
            },
            [] { return ReadRequiredConfiguration(L"PayloadFolder"); },
            [](const AuditPolicy& policy) { return std::make_shared<WindowsSecuritySink>(policy); }};
        g_runtime = std::make_unique<PluginRuntime>(*api, std::move(services));
        hooks->OnVMStarted = OnVMStarted;
        hooks->OnVMStopping = OnVMStopping;
        hooks->OnDistributionStarted = OnDistributionStarted;
        hooks->OnDistributionStopping = OnDistributionStopping;
        if (g_runtime->SupportsWslc())
        {
            hooks->WslcVmStarted = OnWslcVmStarted;
            hooks->WslcVmStopping = OnWslcVmStopping;
            hooks->OnSessionStopping = OnWslcVmStopping;
            hooks->ContainerStarted = OnContainerStarted;
            hooks->ContainerStopping = OnContainerStopping;
        }
        else
        {
            OutputDebugStringW(
                L"WSL Security audit: ordinary WSL enabled; WSLC requires VM lifecycle APIs from WSL 2.9.5 or newer.\n");
        }
        return S_OK;
    }
    CATCH_RETURN();
}
