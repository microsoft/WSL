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

namespace winrt::Microsoft::WSL::Containers::implementation {
winrt::Microsoft::WSL::Containers::ComponentFlags WslcService::GetMissingComponents()
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ServiceVersion WslcService::GetVersion()
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::InstallProgress> WslcService::InstallWithDependenciesAsync()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
