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
    m_session(session)
{
    auto initProcessSettings = GetImplementation(settings)->InitProcess();
    if (initProcessSettings)
    {
        m_initProcessOutputMode = GetImplementation(initProcessSettings)->OutputMode();
    }

    if (m_initProcessOutputMode == ProcessOutputMode::Event)
    {
        m_initProcess = winrt::make_self<implementation::Process>(m_initProcessOutputMode);
        m_initProcess->SetupCallbacksForStart(GetStructPointer(initProcessSettings));
    }

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateContainer(m_session, GetStructPointer(settings), m_container.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::Start()
{
    auto startFlags = (m_initProcessOutputMode == ProcessOutputMode::Stream || m_initProcessOutputMode == ProcessOutputMode::Event)
                          ? WSLC_CONTAINER_START_FLAG_ATTACH
                          : WSLC_CONTAINER_START_FLAG_NONE;

    // Activate callback ownership just before starting: AddRef so the Process stays alive
    // until the exit callback fires. Paired with Release() on the error path.
    bool callbacksActivated = (m_initProcess != nullptr && m_initProcessOutputMode == ProcessOutputMode::Event);
    if (callbacksActivated)
    {
        m_initProcess->ActivateCallbackOwnership();
    }

    auto releaseRef = wil::scope_exit([&] {
        if (callbacksActivated)
        {
            m_initProcess->Release();
        }
    });

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcStartContainer(m_container.get(), startFlags, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);

    m_started = true;

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
        if (m_started)
        {
            WslcProcess initHandle;
            winrt::check_hresult(WslcGetContainerInitProcess(m_container.get(), &initHandle));
            m_initProcess = winrt::make_self<implementation::Process>(initHandle, m_initProcessOutputMode);
        }
        else
        {
            m_initProcess = winrt::make_self<implementation::Process>(m_initProcessOutputMode);
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
    return m_container.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
