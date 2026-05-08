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

PushImageOptions::PushImageOptions(hstring const& image, hstring const& registryAuth) :
    m_image(winrt::to_string(image)), m_registryAuth(winrt::to_string(registryAuth))
{
    if (image.empty() || registryAuth.empty())
    {
        throw hresult_invalid_argument();
    }
}

hstring PushImageOptions::Image()
{
    return winrt::to_hstring(m_image);
}

void PushImageOptions::Image(hstring const& value)
{
    if (m_pushImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_image = winrt::to_string(value);
}

hstring PushImageOptions::RegistryAuth()
{
    return winrt::to_hstring(m_registryAuth);
}

void PushImageOptions::RegistryAuth(hstring const& value)
{
    if (m_pushImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_registryAuth = winrt::to_string(value);
}

WslcPushImageOptions* PushImageOptions::ToStructPointer()
{
    if (m_pushImageOptions)
    {
        return m_pushImageOptions.get();
    }

    m_pushImageOptions = std::make_unique<WslcPushImageOptions>();
    m_pushImageOptions->image = m_image.c_str();
    m_pushImageOptions->registryAuth = m_registryAuth.c_str();
    m_pushImageOptions->progressCallback = nullptr;
    m_pushImageOptions->progressCallbackContext = nullptr;
    return m_pushImageOptions.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
