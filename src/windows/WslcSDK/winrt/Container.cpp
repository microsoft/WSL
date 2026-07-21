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

using namespace winrt::Windows::Foundation;

namespace winrt::Microsoft::WSL::Containers::implementation {
Container::Container(WslcSession session, winrt::Microsoft::WSL::Containers::ContainerSettings const& settings)
{
    if (settings.InitProcess())
    {
        m_initProcess = winrt::make_self<implementation::Process>(settings.InitProcess());
    }

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateContainer(session, GetStructPointer(settings), m_container.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::Start()
{
    auto startFlags = WSLC_CONTAINER_START_FLAG_NONE;
    if (m_initProcess)
    {
        WI_SetFlagIf(
            startFlags,
            WSLC_CONTAINER_START_FLAG_ATTACH,
            m_initProcess->OutputMode() == ProcessOutputMode::Event || m_initProcess->OutputMode() == ProcessOutputMode::Stream);
    }

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcStartContainer(ToHandle(), startFlags, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);

    if (m_initProcess)
    {
        WslcProcess initHandle;
        winrt::check_hresult(WslcGetContainerInitProcess(ToHandle(), &initHandle));
        m_initProcess->AttachHandle(initHandle);
    }
}

void Container::Stop(winrt::Microsoft::WSL::Containers::Signal const& signal, TimeSpan timeout)
{
    wil::unique_cotaskmem_string errorMessage;
    auto timeoutSeconds = std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    if (timeoutSeconds > std::numeric_limits<uint32_t>::max())
    {
        throw winrt::hresult_invalid_argument(L"Timeout is too large");
    }

    if (timeoutSeconds < 0)
    {
        throw winrt::hresult_invalid_argument(L"Timeout must be non-negative");
    }

    auto hr = WslcStopContainer(ToHandle(), static_cast<WslcSignal>(signal), static_cast<uint32_t>(timeoutSeconds), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::Delete(winrt::Microsoft::WSL::Containers::DeleteContainerOption const& flags)
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
        throw winrt::hresult_illegal_method_call(L"This container was not configured with an init process");
    }

    return *m_initProcess;
}

winrt::Microsoft::WSL::Containers::ContainerState Container::State()
{
    WslcContainerState state;
    winrt::check_hresult(WslcGetContainerState(ToHandle(), &state));
    return static_cast<winrt::Microsoft::WSL::Containers::ContainerState>(state);
}

void Container::EnsureNotClosed() const
{
    if (!m_container)
    {
        throw winrt::hresult_error(RO_E_CLOSED, L"Container has been closed");
    }
}

WslcContainer Container::ToHandle()
{
    EnsureNotClosed();
    return m_container.get();
}

void Container::Close()
{
    m_initProcess = nullptr;

    // Methods called after Close() will fail due to EnsureNotClosed().
    m_container.reset();
}

void Container::final_release(std::unique_ptr<Container> self)
{
    // Ensure cleanup when refcount drops to zero even if Close() was not called explicitly.
    self->Close();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
