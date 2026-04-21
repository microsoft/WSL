/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PullImageOptions.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK PullImageOptions class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.PullImageOptions.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct PullImageOptions : PullImageOptionsT<PullImageOptions>
{
    PullImageOptions() = default;

    PullImageOptions(hstring const& uri);
    hstring Uri();
    void Uri(hstring const& value);
    hstring RegistryAuth();
    void RegistryAuth(hstring const& value);

    WslcPullImageOptions* ToStructPointer();

private:
    std::string m_uri;
    std::string m_registryAuth;

    std::unique_ptr<WslcPullImageOptions> m_pullImageOptions;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct PullImageOptions : PullImageOptionsT<PullImageOptions, implementation::PullImageOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(PullImageOptions);
