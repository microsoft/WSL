/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Container.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Container class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Container.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Container : ContainerT<Container>
{
    Container() = default;

    void Start(winrt::Microsoft::WSL::Containers::ContainerStartFlags const& flags);
    void Stop(winrt::Microsoft::WSL::Containers::Signal const& signal, uint32_t timeoutSeconds);
    void Delete(winrt::Microsoft::WSL::Containers::DeleteContainerFlags const& flags);
    winrt::Microsoft::WSL::Containers::Process CreateProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& newProcessSettings);
    hstring Inspect();
    hstring Id();
    winrt::Microsoft::WSL::Containers::Process InitProcess();
    winrt::Microsoft::WSL::Containers::ContainerState State();
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
