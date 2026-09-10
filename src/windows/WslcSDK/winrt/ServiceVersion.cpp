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
ServiceVersion::ServiceVersion(uint32_t major, uint32_t minor, uint32_t revision) :
    m_major(major), m_minor(minor), m_revision(revision)
{
}

uint32_t ServiceVersion::Major()
{
    return m_major;
}

uint32_t ServiceVersion::Minor()
{
    return m_minor;
}

uint32_t ServiceVersion::Revision()
{
    return m_revision;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
