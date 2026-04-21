/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Session.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Session class.

--*/

#include "precomp.h"
#include "Session.h"
#include "Microsoft.WSL.Containers.Session.g.cpp"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::WSL::Containers::implementation {
Session::Session(WslcSession session) : m_session(session)
{
}

winrt::Microsoft::WSL::Containers::Session Session::Create(winrt::Microsoft::WSL::Containers::SessionSettings const& settings)
{
    wil::unique_cotaskmem_string errorMessage;
    WslcSession sessionHandle;
    auto hr = WslcCreateSession(GetStructPointer(settings), &sessionHandle, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return *winrt::make_self<implementation::Session>(sessionHandle);
}

void Session::Terminate()
{
    winrt::check_hresult(WslcTerminateSession(ToHandle()));
}

winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PullImageAsync(
    winrt::Microsoft::WSL::Containers::PullImageOptions options)
{
    co_await winrt::resume_background();
    throw hresult_not_implemented();
}

winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::ImportImageAsync(hstring path, hstring imageName)
{
    co_await winrt::resume_background();
    throw hresult_not_implemented();
}

winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::LoadImageAsync(hstring path)
{
    co_await winrt::resume_background();
    throw hresult_not_implemented();
}

winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PushImageAsync(
    winrt::Microsoft::WSL::Containers::PushImageOptions options)
{
    co_await winrt::resume_background();
    throw hresult_not_implemented();
}

void Session::DeleteImage(hstring const& nameOrId)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDeleteSessionImage(ToHandle(), winrt::to_string(nameOrId).c_str(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::TagImage(winrt::Microsoft::WSL::Containers::TagImageOptions const& options)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcTagSessionImage(ToHandle(), GetStructPointer(options), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdRequirements const& options)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateSessionVhdVolume(ToHandle(), GetStructPointer(options), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::DeleteVhdVolume(hstring const& name)
{
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDeleteSessionVhdVolume(ToHandle(), winrt::to_string(name).c_str(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

hstring Session::Authenticate(Uri const& serverAddress, hstring const& username, hstring const& password)
{
    wil::unique_cotaskmem_string errorMessage;
    wil::unique_cotaskmem_ansistring token;
    auto hr = WslcSessionAuthenticate(ToHandle(), winrt::to_string(serverAddress.ToString()).c_str(), winrt::to_string(username).c_str(), winrt::to_string(password).c_str(), token.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return winrt::to_hstring(token.get());

}

IVectorView<winrt::Microsoft::WSL::Containers::ImageInfo> Session::Images()
{
    wil::unique_cotaskmem_array_ptr<WslcImageInfo> imagesArray;
    uint32_t count = 0;
    winrt::check_hresult(WslcListSessionImages(ToHandle(), imagesArray.put(), &count));

    auto images = winrt::single_threaded_vector<winrt::Microsoft::WSL::Containers::ImageInfo>();
    for (uint32_t i = 0; i < count; i++)
    {
        images.Append(winrt::make<implementation::ImageInfo>(imagesArray[i]));
    }

    return images.GetView();
}

WslcSession Session::ToHandle()
{
    return m_session.get();
}

WslcSession* Session::ToHandlePointer()
{
    return m_session.addressof();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
