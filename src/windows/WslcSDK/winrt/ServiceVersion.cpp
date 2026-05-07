/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceVersion.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ServiceVersion class.

--*/

#include "precomp.h"
#include "ServiceVersion.h"
#include "Microsoft.WSL.Containers.ServiceVersion.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
uint32_t ServiceVersion::Major()
{
    throw hresult_not_implemented();
}
uint32_t ServiceVersion::Minor()
{
    throw hresult_not_implemented();
}
uint32_t ServiceVersion::Revision()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
