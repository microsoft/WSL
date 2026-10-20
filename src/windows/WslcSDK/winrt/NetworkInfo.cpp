/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkInfo.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK NetworkInfo class.

--*/

#include "precomp.h"
#include "NetworkInfo.h"
#include "Microsoft.WSL.Containers.NetworkInfo.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
NetworkInfo::NetworkInfo(WslcNetworkInfo const& info)
{
    m_id = winrt::to_hstring(info.id);
    m_name = winrt::to_hstring(info.name);
    m_driver = winrt::to_hstring(info.driver);
    m_scope = winrt::to_hstring(info.scope);
    m_createdTimestamp = winrt::clock::from_time_t(static_cast<time_t>(info.createdUnixTime));
    m_enableIPv4 = info.enableIPv4 != FALSE;
    m_enableIPv6 = info.enableIPv6 != FALSE;
    m_internal = info.internal != FALSE;
}

hstring NetworkInfo::Id()
{
    return m_id;
}

hstring NetworkInfo::Name()
{
    return m_name;
}

hstring NetworkInfo::Driver()
{
    return m_driver;
}

hstring NetworkInfo::Scope()
{
    return m_scope;
}

winrt::Windows::Foundation::DateTime NetworkInfo::CreatedTimestamp()
{
    return m_createdTimestamp;
}

bool NetworkInfo::EnableIPv4()
{
    return m_enableIPv4;
}

bool NetworkInfo::EnableIPv6()
{
    return m_enableIPv6;
}

bool NetworkInfo::Internal()
{
    return m_internal;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
