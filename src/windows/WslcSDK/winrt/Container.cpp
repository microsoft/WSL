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
#include "ContainerSettings.h"
#include "Process.h"
#include "ProcessSettings.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
Container::Container(WslcSession session, winrt::Microsoft::WSL::Containers::ContainerSettings const& settings) :
    m_session(session), m_settings(settings)
{
}

void Container::Start(winrt::Microsoft::WSL::Containers::ContainerStartFlags const& flags)
{
    auto startFlags = static_cast<WslcContainerStartFlags>(flags);

    // Apply callbacks BEFORE EnsureCreated: WslcCreateContainer copies the init process settings,
    // so the callbacks must already be set on the ProcessSettings struct at that point.
    bool callbacksApplied = false;
    if (m_initProcess)
    {
        callbacksApplied = m_initProcess->ApplyCallbacksToSettings(GetStructPointer(GetImplementation(m_settings)->InitProcess()));
        THROW_HR_IF(E_INVALIDARG, callbacksApplied && !WI_IsFlagSet(startFlags, WSLC_CONTAINER_START_FLAG_ATTACH));
    }

    auto releaseRef = wil::scope_exit([&] {
        if (callbacksApplied)
        {
            m_initProcess->Release();
        }
    });

    EnsureCreated();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcStartContainer(m_container.get(), startFlags, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);

    if (m_initProcess)
    {
        WslcProcess initHandle;
        winrt::check_hresult(WslcGetContainerInitProcess(m_container.get(), &initHandle));
        m_initProcess->AttachHandle(initHandle);
    }

    releaseRef.release();
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
    return *winrt::make_self<implementation::Process>(*this, newProcessSettings);
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
    if (!m_initProcess)
    {
        if (m_container)
        {
            WslcProcess initHandle;
            winrt::check_hresult(WslcGetContainerInitProcess(m_container.get(), &initHandle));
            m_initProcess = winrt::make_self<implementation::Process>(initHandle);
        }
        else
        {
            m_initProcess = winrt::make_self<implementation::Process>();
        }
    }

    return *m_initProcess;
}

winrt::Microsoft::WSL::Containers::ContainerState Container::State()
{
    WslcContainerState state;
    winrt::check_hresult(WslcGetContainerState(ToHandle(), &state));
    return static_cast<winrt::Microsoft::WSL::Containers::ContainerState>(state);
}

WslcContainer Container::ToHandle()
{
    EnsureCreated();
    return m_container.get();
}

void Container::EnsureCreated()
{
    if (!m_container)
    {
        wil::unique_cotaskmem_string errorMessage;
        auto hr = WslcCreateContainer(m_session, GetStructPointer(m_settings), m_container.put(), errorMessage.put());
        THROW_MSG_IF_FAILED(hr, errorMessage);
    }
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
