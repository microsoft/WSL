/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PushImageOptions.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK PushImageOptions class.

--*/

#include "precomp.h"
#include "PushImageOptions.h"
#include "Microsoft.WSL.Containers.PushImageOptions.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
PushImageOptions::PushImageOptions(hstring const& image, hstring const& registryAuth)
{
    throw hresult_not_implemented();
}
hstring PushImageOptions::Image()
{
    throw hresult_not_implemented();
}
void PushImageOptions::Image(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring PushImageOptions::RegistryAuth()
{
    throw hresult_not_implemented();
}
void PushImageOptions::RegistryAuth(hstring const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
