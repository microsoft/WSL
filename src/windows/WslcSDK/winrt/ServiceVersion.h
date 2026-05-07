/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceVersion.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ServiceVersion class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ServiceVersion.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ServiceVersion : ServiceVersionT<ServiceVersion>
{
    ServiceVersion() = default;

    uint32_t Major();
    uint32_t Minor();
    uint32_t Revision();
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
