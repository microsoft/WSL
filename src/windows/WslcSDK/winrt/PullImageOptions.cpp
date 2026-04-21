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

PullImageOptions::PullImageOptions(hstring const& uri) :
    m_uri(winrt::to_string(uri))
{
}

hstring PullImageOptions::Uri()
{
    return winrt::to_hstring(m_uri);
}

void PullImageOptions::Uri(hstring const& value)
{
    if (m_pullImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    m_uri = winrt::to_string(value);
}

hstring PullImageOptions::RegistryAuth()
{
    return winrt::to_hstring(m_registryAuth);
}

void PullImageOptions::RegistryAuth(hstring const& value)
{
    if (m_pullImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    m_registryAuth = winrt::to_string(value);
}

WslcPullImageOptions* PullImageOptions::ToStructPointer()
{
    if (m_pullImageOptions)
    {
        return m_pullImageOptions.get();
    }

    m_pullImageOptions = std::make_unique<WslcPullImageOptions>();
    m_pullImageOptions->uri = m_uri.c_str();
    m_pullImageOptions->registryAuth = m_registryAuth.empty() ? nullptr : m_registryAuth.c_str();
    m_pullImageOptions->progressCallback = nullptr;
    m_pullImageOptions->progressCallbackContext = nullptr;
    return m_pullImageOptions.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
