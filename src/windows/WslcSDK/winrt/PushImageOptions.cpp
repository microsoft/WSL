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
    if (image.empty())
    {
        throw hresult_invalid_argument(L"Image cannot be empty");
    }

    if (registryAuth.empty())
    {
        throw hresult_invalid_argument(L"Registry auth cannot be empty");
    }
}

hstring PushImageOptions::Image()
{
    return winrt::to_hstring(m_image);
}

void PushImageOptions::Image(hstring const& value)
{
    if (value.empty())
    {
        throw hresult_invalid_argument(L"Image cannot be empty");
    }

    m_image = winrt::to_string(value);
}

hstring PushImageOptions::RegistryAuth()
{
    return winrt::to_hstring(m_registryAuth);
}

void PushImageOptions::RegistryAuth(hstring const& value)
{
    if (value.empty())
    {
        throw hresult_invalid_argument(L"Registry auth cannot be empty");
    }

    m_registryAuth = winrt::to_string(value);
}

WslcPushImageOptions PushImageOptions::ToStruct()
{
    WslcPushImageOptions pushImageOptions{};
    pushImageOptions.image = m_image.c_str();
    pushImageOptions.registryAuth = m_registryAuth.c_str();
    pushImageOptions.progressCallback = nullptr;
    pushImageOptions.progressCallbackContext = nullptr;
    return pushImageOptions;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
