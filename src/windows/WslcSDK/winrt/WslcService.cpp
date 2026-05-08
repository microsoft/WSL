/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcService.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK WslcService class.

--*/

#include "precomp.h"
#include "WslcService.h"
#include "Microsoft.WSL.Containers.WslcService.g.cpp"
#include "ServiceVersion.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
winrt::Microsoft::WSL::Containers::ComponentFlags WslcService::GetMissingComponents()
{
    WslcComponentFlags missing;
    winrt::check_hresult(WslcGetMissingComponents(&missing));
    return static_cast<winrt::Microsoft::WSL::Containers::ComponentFlags>(missing);
}

winrt::Microsoft::WSL::Containers::ServiceVersion WslcService::GetVersion()
{
    WslcVersion version;
    winrt::check_hresult(WslcGetVersion(&version));
    return winrt::make<implementation::ServiceVersion>(version.major, version.minor, version.revision);
}

winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::InstallProgress> WslcService::InstallWithDependenciesAsync()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
