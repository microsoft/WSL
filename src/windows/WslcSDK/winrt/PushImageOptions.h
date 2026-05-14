/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PushImageOptions.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK PushImageOptions class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.PushImageOptions.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct PushImageOptions : PushImageOptionsT<PushImageOptions>
{
    PushImageOptions() = default;

    PushImageOptions(hstring const& image, hstring const& registryAuth);
    hstring Image();
    void Image(hstring const& value);
    hstring RegistryAuth();
    void RegistryAuth(hstring const& value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct PushImageOptions : PushImageOptionsT<PushImageOptions, implementation::PushImageOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
