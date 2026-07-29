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
    WslcComponentFlags GetComponentsForInstall(const InstallOptions& options)
    {
        WslcComponentFlags result = WslcComponentFlags::WSLC_COMPONENT_FLAG_NONE;
        bool shouldCheckMissingComponents = true;

        if (options)
        {
            auto components = options.Components();
            if (components)
            {
                shouldCheckMissingComponents = false;

                for (const auto& component : components)
                {
                    switch (component)
                    {
                    case Component::VirtualMachinePlatform:
                        result |= WslcComponentFlags::WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM;
                        break;
                    case Component::WslPackage:
                        result |= WslcComponentFlags::WSLC_COMPONENT_FLAG_WSL_PACKAGE;
                        break;
                    case Component::SdkNeedsUpdate:
                        THROW_HR(WSLC_E_SDK_UPDATE_NEEDED);
                    default:
                        THROW_HR(E_INVALIDARG);
                    }
                }
            }
        }

        if (shouldCheckMissingComponents)
        {
            winrt::check_hresult(WslcGetMissingComponents(&result));
        }

        return result;
    }

    WslcInstallOptions GetOptionsForInstall(const InstallOptions& options)
    {
        WslcInstallOptions result = WslcInstallOptions::WSLC_INSTALL_OPTION_NONE;

        if (options)
        {
            if (options.Repair())
            {
                result |= WslcInstallOptions::WSLC_INSTALL_OPTION_REPAIR;
            }
        }

        return result;
    }

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

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::InstallProgress> WslcService::InstallWithDependenciesAsync(
    winrt::Microsoft::WSL::Containers::InstallOptions options)
{
    auto components = GetComponentsForInstall(options);
    auto wslcOptions = GetOptionsForInstall(options);

    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::InstallProgress>{co_await winrt::get_progress_token()};
    winrt::check_hresult(WslcInstallWithDependencies(components, wslcOptions, InstallProgressCallback, &context));
}

void WslcService::InstallWithDependencies(winrt::Microsoft::WSL::Containers::InstallOptions options)
{
    auto components = GetComponentsForInstall(options);
    auto wslcOptions = GetOptionsForInstall(options);
    winrt::check_hresult(WslcInstallWithDependencies(components, wslcOptions, nullptr, nullptr));
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
