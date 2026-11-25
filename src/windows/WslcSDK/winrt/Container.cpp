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
#include "ContainerNetwork.h"
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

    RegisterInitProcessCallbacks();
}

Container::Container(WslcContainer handle, winrt::Microsoft::WSL::Containers::ProcessOutputMode initProcessOutputMode) :
    m_opened(true)
{
    m_container.reset(handle);

    m_initProcess = winrt::make_self<implementation::Process>(initProcessOutputMode);

    RegisterInitProcessCallbacks();

    // Best-effort: attach the init process handle if one is available. The container may not
    // have been started yet, or may not have an init process at all.
    WslcProcess initHandle{};
    if (SUCCEEDED(WslcGetContainerInitProcess(m_container.get(), &initHandle)) && initHandle)
    {
        m_initProcess->AttachHandle(initHandle);
    }
}

void Container::RegisterInitProcessCallbacks()
{
    // Callbacks are registered on the container handle so that a ProcessSettings shared with other processes is never modified.
    if (!m_initProcess || m_initProcess->OutputMode() != ProcessOutputMode::Event)
    {
        return;
    }

    auto callbacks = m_initProcess->GetEventCallbacks();
    winrt::check_hresult(WslcSetContainerInitProcessIOCallbacks(m_container.get(), &callbacks, m_initProcess.get()));
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
        WslcProcess initHandle{};
        hr = WslcGetContainerInitProcess(ToHandle(), &initHandle);
        THROW_HR_IF(hr, !m_opened && FAILED(hr));
        if (SUCCEEDED(hr) && initHandle)
        {
            m_initProcess->AttachHandle(initHandle);
        }
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

void Container::ConnectNetwork(winrt::Microsoft::WSL::Containers::ContainerNetwork const& network)
{
    if (!network)
    {
        throw winrt::hresult_error(E_POINTER, L"Network cannot be null");
    }

    const auto networkStruct = GetStruct(network);
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcConnectContainerNetwork(ToHandle(), &networkStruct, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Container::DisconnectNetwork(hstring const& nameOrId)
{
    if (nameOrId.empty())
    {
        throw winrt::hresult_invalid_argument(L"Network name or ID cannot be empty");
    }

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDisconnectContainerNetwork(ToHandle(), winrt::to_string(nameOrId).c_str(), errorMessage.put());
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
    // Keep the init process callback context alive until native container release succeeds.
    // EnsureNotClosed() rejects native operations after a successful Close().
    if (m_container)
    {
        winrt::check_hresult(WslcReleaseContainer(m_container.get()));
        std::ignore = m_container.release();
    }

    m_initProcess = nullptr;
}

void Container::final_release(std::unique_ptr<Container> self) noexcept
{
    // Clean up when the last reference is released, including clients that omit Close().
    try
    {
        self->Close();
        return;
    }
    catch (winrt::hresult_error const& error)
    {
        FAIL_FAST_HR_IF(static_cast<HRESULT>(error.code()), error.code() != HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE_STATE));
    }

    // Defer cleanup to a worker that can wait for the IO callback to return.
    ReleaseOnBackgroundThread(std::move(self));
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
