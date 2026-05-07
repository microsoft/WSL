/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProcessSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ProcessSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ProcessSettings.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ProcessSettings : ProcessSettingsT<ProcessSettings>
{
    ProcessSettings() = default;

    hstring WorkingDirectory();
    void WorkingDirectory(hstring const& value);
    winrt::Windows::Foundation::Collections::IVector<hstring> CmdLine();
    void CmdLine(winrt::Windows::Foundation::Collections::IVector<hstring> const& value);
    winrt::Windows::Foundation::Collections::IMap<hstring, hstring> EnvironmentVariables();
    void EnvironmentVariables(winrt::Windows::Foundation::Collections::IMap<hstring, hstring> const& value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ProcessSettings : ProcessSettingsT<ProcessSettings, implementation::ProcessSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
