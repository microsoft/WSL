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

PullImageOptions::PullImageOptions(hstring const& uri) : m_uri(winrt::to_string(uri))
{
    if (uri.empty())
    {
        throw hresult_invalid_argument(L"URI cannot be empty");
    }
}

hstring PullImageOptions::Uri()
{
    return winrt::to_hstring(m_uri);
}

void PullImageOptions::Uri(hstring const& value)
{
    if (m_pullImageOptions)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value.empty())
    {
        throw hresult_invalid_argument(L"URI cannot be empty");
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
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_registryAuth = winrt::to_string(value);
}

WslcPullImageOptions PullImageOptions::ToStruct()
{
    if (!m_pullImageOptions)
    {
        m_pullImageOptions = std::make_unique<WslcPullImageOptions>();
        m_pullImageOptions->uri = m_uri.c_str();
        m_pullImageOptions->registryAuth = m_registryAuth.empty() ? nullptr : m_registryAuth.c_str();
        m_pullImageOptions->progressCallback = nullptr;
        m_pullImageOptions->progressCallbackContext = nullptr;
    }

    return *m_pullImageOptions;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
