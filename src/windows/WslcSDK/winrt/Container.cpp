/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Container.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Container class.

--*/

#include "precomp.h"
#include "Container.h"
#include "Microsoft.WSL.Containers.Container.g.cpp"
#include "Process.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
Container::Container(WslcContainer container)
    : m_container(container)
{
}

winrt::Microsoft::WSL::Containers::Container Container::Create(
    winrt::Microsoft::WSL::Containers::Session const& session, winrt::Microsoft::WSL::Containers::ContainerSettings const& containerSettings)
{
    wil::unique_cotaskmem_string errorMessage;
    WslcContainer containerHandle;
    auto hr = WslcCreateContainer(GetHandle(session), GetStructPointer(containerSettings), &containerHandle, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return *winrt::make_self<implementation::Container>(containerHandle);
}

void Container::Start(winrt::Microsoft::WSL::Containers::ContainerStartFlags const& flags)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcStartContainer(ToHandle(), static_cast<WslcContainerStartFlags>(flags), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::Stop(winrt::Microsoft::WSL::Containers::Signal const& signal, uint32_t timeoutSeconds)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcStopContainer(ToHandle(), static_cast<WslcSignal>(signal), timeoutSeconds, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::Delete(winrt::Microsoft::WSL::Containers::DeleteContainerFlags const& flags)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDeleteContainer(ToHandle(), static_cast<WslcDeleteContainerFlags>(flags), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

winrt::Microsoft::WSL::Containers::Process Container::CreateProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& newProcessSettings)
{
    wil::unique_cotaskmem_string errorMessage;
    WslcProcess process;
    auto hr = WslcCreateContainerProcess(ToHandle(), GetStructPointer(newProcessSettings), &process, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return *winrt::make_self<implementation::Process>(process);
}

hstring Container::Inspect()
{
    wil::unique_cotaskmem_ansistring inspectData;
    winrt::check_hresult(WslcInspectContainer(ToHandle(), inspectData.put()));
    return winrt::to_hstring(inspectData.get());
}

hstring Container::Id()
{
    CHAR id[WSLC_CONTAINER_ID_BUFFER_SIZE];
    winrt::check_hresult(WslcGetContainerID(ToHandle(), id));
    return winrt::to_hstring(id);
}

winrt::Microsoft::WSL::Containers::Process Container::InitProcess()
{
    WslcProcess initProcess;
    winrt::check_hresult(WslcGetContainerInitProcess(ToHandle(), &initProcess));
    return *winrt::make_self<implementation::Process>(initProcess);
}

winrt::Microsoft::WSL::Containers::ContainerState Container::State()
{
    WslcContainerState state;
    winrt::check_hresult(WslcGetContainerState(ToHandle(), &state));
    return static_cast<winrt::Microsoft::WSL::Containers::ContainerState>(state);
}

WslcContainer Container::ToHandle()
{
    return m_container.get();
}

WslcContainer* Container::ToHandlePointer()
{
    return m_container.addressof();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
