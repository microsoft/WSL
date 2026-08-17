/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCComposeSession.cpp

Abstract:

    Implements the minimal compose session COM object.

--*/

#include "precomp.h"
#include "WSLCComposeSession.h"

namespace wsl::windows::service::wslc {

HRESULT WSLCComposeSession::RuntimeClassInitialize(std::wstring ConfigPath, std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Containers)
{
    RETURN_HR_IF(E_INVALIDARG, ConfigPath.empty());
    RETURN_HR_IF(E_INVALIDARG, Containers.empty());

    m_configPath = std::move(ConfigPath);
    m_containers = std::move(Containers);
    return S_OK;
}

HRESULT WSLCComposeSession::GetConfigPath(LPWSTR* Path)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Path);
    *Path = nullptr;

    std::lock_guard lock(m_lock);
    *Path = wil::make_cotaskmem_string(m_configPath.c_str()).release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeSession::ListContainers(WSLCContainerEntry** Containers, ULONG* Count)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Containers);
    RETURN_HR_IF_NULL(E_POINTER, Count);
    *Containers = nullptr;
    *Count = 0;

    std::lock_guard lock(m_lock);
    auto result = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(m_containers.size());
    for (size_t index = 0; index < m_containers.size(); ++index)
    {
        auto& entry = result[index];
        wil::unique_cotaskmem_ansistring name;
        THROW_IF_FAILED(m_containers[index]->GetName(&name));
        THROW_IF_FAILED(m_containers[index]->GetId(entry.Id));
        THROW_IF_FAILED(m_containers[index]->GetState(&entry.State));
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(entry.Name, name.get()) != 0);

        wil::unique_cotaskmem_ansistring inspect;
        THROW_IF_FAILED(m_containers[index]->Inspect(&inspect));
        const auto json = nlohmann::json::parse(inspect.get());
        const auto image = json.value("Image", std::string{});
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(entry.Image, image.c_str()) != 0);
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = result.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeSession::Start()
try
{
    std::lock_guard lock(m_lock);
    for (const auto& container : m_containers)
    {
        const HRESULT result = container->Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
        THROW_IF_FAILED_EXCEPT(result, WSLC_E_CONTAINER_IS_RUNNING);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeSession::Attach()
{
    // Stream transport remains client-owned. The CLI uses ListContainers() and the existing
    // IWSLCContainer::Attach handle transport after validating this service-tracked session.
    return S_OK;
}

HRESULT WSLCComposeSession::Stop(ULONG Timeout)
try
{
    THROW_HR_IF(E_INVALIDARG, Timeout > LONG_MAX);

    std::lock_guard lock(m_lock);
    HRESULT firstFailure = S_OK;
    for (const auto& container : m_containers)
    {
        const HRESULT result = container->Stop(WSLCSignalSIGTERM, static_cast<LONG>(Timeout));
        if (FAILED(result) && result != WSLC_E_CONTAINER_NOT_RUNNING && SUCCEEDED(firstFailure))
        {
            firstFailure = result;
        }
    }

    return firstFailure;
}
CATCH_RETURN();

HRESULT WSLCComposeSession::InterfaceSupportsErrorInfo(REFIID InterfaceId)
{
    return InterfaceId == __uuidof(IWSLCComposeSession) ? S_OK : S_FALSE;
}

} // namespace wsl::windows::service::wslc
