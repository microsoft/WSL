/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcService.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK WslcService class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.WslcService.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct WslcService
{
    WslcService() = default;

    static winrt::Microsoft::WSL::Containers::ComponentFlags GetMissingComponents();
    static winrt::Microsoft::WSL::Containers::ServiceVersion GetVersion();
    static winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::InstallProgress> InstallWithDependenciesAsync();
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct WslcService : WslcServiceT<WslcService, implementation::WslcService>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(WslcService);
