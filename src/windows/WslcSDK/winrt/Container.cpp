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

namespace winrt::Microsoft::WSL::Containers::implementation {
void Container::Start(winrt::Microsoft::WSL::Containers::ContainerStartFlags const& flags)
{
    throw hresult_not_implemented();
}
void Container::Stop(winrt::Microsoft::WSL::Containers::Signal const& signal, uint32_t timeoutSeconds)
{
    throw hresult_not_implemented();
}
void Container::Delete(winrt::Microsoft::WSL::Containers::DeleteContainerFlags const& flags)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::Process Container::CreateProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& newProcessSettings)
{
    throw hresult_not_implemented();
}
hstring Container::Inspect()
{
    throw hresult_not_implemented();
}
hstring Container::Id()
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::Process Container::InitProcess()
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ContainerState Container::State()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
