/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallOptions.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK InstallOptions class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.InstallOptions.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct InstallOptions : InstallOptionsT<InstallOptions>
{
    InstallOptions() = default;

    winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> Components();
    void Components(winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> value);
    bool Repair();
    void Repair(bool value);

private:
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> m_components = nullptr;
    bool m_repair = false;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct InstallOptions : InstallOptionsT<InstallOptions, implementation::InstallOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(InstallOptions);
