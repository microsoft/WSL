/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PullImageOptions.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK PullImageOptions class.

--*/

#include "precomp.h"
#include "PullImageOptions.h"
#include "Microsoft.WSL.Containers.PullImageOptions.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
PullImageOptions::PullImageOptions(hstring const& uri)
{
    throw hresult_not_implemented();
}
hstring PullImageOptions::Uri()
{
    throw hresult_not_implemented();
}
void PullImageOptions::Uri(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring PullImageOptions::RegistryAuth()
{
    throw hresult_not_implemented();
}
void PullImageOptions::RegistryAuth(hstring const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
