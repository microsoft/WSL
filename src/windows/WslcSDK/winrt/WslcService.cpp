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
#include "InstallProgress.h"

using namespace winrt::Windows::Foundation;

namespace winrt::Microsoft::WSL::Containers::implementation {

namespace {
    void CALLBACK InstallProgressCallback(WslcComponentFlags component, uint32_t progressSteps, uint32_t totalSteps, PVOID context) noexcept
    {
        try
        {
            auto installProgress = winrt::make<implementation::InstallProgress>(
                static_cast<winrt::Microsoft::WSL::Containers::ComponentFlags>(component), progressSteps, totalSteps);
            ProgressCallbackHelper<decltype(installProgress)>::ReportProgress(context, installProgress);
        }
        CATCH_LOG();
    }
} // namespace

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

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::InstallProgress> WslcService::InstallWithDependenciesAsync()
{
    co_await winrt::resume_background();
    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::InstallProgress>{co_await winrt::get_progress_token()};
    winrt::check_hresult(WslcInstallWithDependencies(InstallProgressCallback, &context));
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
