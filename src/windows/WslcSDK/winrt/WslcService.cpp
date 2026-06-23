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
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::WSL::Containers::implementation {

namespace {
    void CALLBACK InstallProgressCallback(WslcComponentFlags component, uint32_t progressSteps, uint32_t totalSteps, PVOID context) noexcept
    {
        try
        {
            auto installProgress = winrt::make<implementation::InstallProgress>(
                static_cast<winrt::Microsoft::WSL::Containers::Component>(component), progressSteps, totalSteps);
            ProgressCallbackHelper<decltype(installProgress)>::ReportProgress(context, installProgress);
        }
        CATCH_LOG();
    }
} // namespace

winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::Component> WslcService::GetMissingComponents()
{
    WslcComponentFlags missing;
    winrt::check_hresult(WslcGetMissingComponents(&missing));

    auto result = winrt::single_threaded_vector<winrt::Microsoft::WSL::Containers::Component>();
    if (WI_IsFlagSet(missing, WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM))
    {
        result.Append(winrt::Microsoft::WSL::Containers::Component::VirtualMachinePlatform);
    }
    if (WI_IsFlagSet(missing, WSLC_COMPONENT_FLAG_WSL_PACKAGE))
    {
        result.Append(winrt::Microsoft::WSL::Containers::Component::WslPackage);
    }
    if (WI_IsFlagSet(missing, WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE))
    {
        result.Append(winrt::Microsoft::WSL::Containers::Component::SdkNeedsUpdate);
    }
    return result.GetView();
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

void WslcService::InstallWithDependencies()
{
    winrt::check_hresult(WslcInstallWithDependencies(nullptr, nullptr));
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
