/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceVersion.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ServiceVersion class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ServiceVersion.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ServiceVersion : ServiceVersionT<ServiceVersion>
{
    ServiceVersion() = default;
    ServiceVersion(uint32_t major, uint32_t minor, uint32_t revision);

    uint32_t Major();
    uint32_t Minor();
    uint32_t Revision();

private:
    uint32_t m_major{};
    uint32_t m_minor{};
    uint32_t m_revision{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(ServiceVersion);
