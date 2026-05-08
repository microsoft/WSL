/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Container.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Container class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Container.g.h"
#include "Helpers.h"
#include "Process.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Container : ContainerT<Container>
{
    Container() = default;
    Container(WslcSession session, winrt::Microsoft::WSL::Containers::ContainerSettings const& settings);

    void Start(winrt::Microsoft::WSL::Containers::ContainerStartFlags const& flags);
    void Stop(winrt::Microsoft::WSL::Containers::Signal const& signal, uint32_t timeoutSeconds);
    void Delete(winrt::Microsoft::WSL::Containers::DeleteContainerFlags const& flags);
    winrt::Microsoft::WSL::Containers::Process CreateProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& newProcessSettings);
    hstring Inspect();
    hstring Id();
    winrt::Microsoft::WSL::Containers::Process InitProcess();
    winrt::Microsoft::WSL::Containers::ContainerState State();

    WslcContainer ToHandle();

private:
    void EnsureCreated();

    WslcSession m_session{};
    winrt::Microsoft::WSL::Containers::ContainerSettings m_settings{nullptr};
    wil::unique_any<WslcContainer, decltype(&WslcReleaseContainer), &WslcReleaseContainer> m_container;
    winrt::com_ptr<implementation::Process> m_initProcess;
};

} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(Container);
