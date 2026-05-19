/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK SessionSettings class.

--*/

#include "precomp.h"
#include "SessionSettings.h"
#include "Microsoft.WSL.Containers.SessionSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
SessionSettings::SessionSettings(hstring const& name, hstring const& storagePath)
{
    throw hresult_not_implemented();
}
hstring SessionSettings::Name()
{
    throw hresult_not_implemented();
}
void SessionSettings::Name(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring SessionSettings::StoragePath()
{
    throw hresult_not_implemented();
}
void SessionSettings::StoragePath(hstring const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::CpuCount()
{
    throw hresult_not_implemented();
}
void SessionSettings::CpuCount(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::MemoryMB()
{
    throw hresult_not_implemented();
}
void SessionSettings::MemoryMB(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::TimeoutMS()
{
    throw hresult_not_implemented();
}
void SessionSettings::TimeoutMS(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::VhdOptions SessionSettings::VhdRequirements()
{
    throw hresult_not_implemented();
}
void SessionSettings::VhdRequirements(winrt::Microsoft::WSL::Containers::VhdOptions const& value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::SessionFeatureFlags SessionSettings::FeatureFlags()
{
    throw hresult_not_implemented();
}
void SessionSettings::FeatureFlags(winrt::Microsoft::WSL::Containers::SessionFeatureFlags const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
