/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallOptions.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK InstallOptions class.

--*/

#include "precomp.h"
#include "InstallOptions.h"
#include "Microsoft.WSL.Containers.InstallOptions.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> InstallOptions::Components()
{
    return m_components;
}

void InstallOptions::Components(winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> value)
{
    m_components = std::move(value);
}

bool InstallOptions::Repair()
{
    return m_repair;
}

void InstallOptions::Repair(bool value)
{
    m_repair = value;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
