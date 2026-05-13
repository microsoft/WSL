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

namespace winrt::Microsoft::WSL::Containers::implementation {
Session::Session(winrt::Microsoft::WSL::Containers::SessionSettings const& settings)
{
    throw hresult_not_implemented();
}
void Session::Start()
{
    throw hresult_not_implemented();
}
void Session::Terminate()
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::Container Session::CreateContainer(winrt::Microsoft::WSL::Containers::ContainerSettings const& containerSettings)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PullImageAsync(
    winrt::Microsoft::WSL::Containers::PullImageOptions options)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::ImportImageAsync(hstring path, hstring imageName)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::LoadImageAsync(hstring path)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PushImageAsync(
    winrt::Microsoft::WSL::Containers::PushImageOptions options)
{
    throw hresult_not_implemented();
}
void Session::DeleteImage(hstring const& nameOrId)
{
    throw hresult_not_implemented();
}
void Session::TagImage(winrt::Microsoft::WSL::Containers::TagImageOptions const& options)
{
    throw hresult_not_implemented();
}
void Session::CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdOptions const& options)
{
    throw hresult_not_implemented();
}
void Session::DeleteVhdVolume(hstring const& name)
{
    throw hresult_not_implemented();
}
hstring Session::Authenticate(winrt::Windows::Foundation::Uri const& serverAddress, hstring const& username, hstring const& password)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::ImageInfo> Session::Images()
{
    throw hresult_not_implemented();
}
winrt::event_token Session::Terminated(winrt::Microsoft::WSL::Containers::SessionTerminationHandler const& handler)
{
    throw hresult_not_implemented();
}
void Session::Terminated(winrt::event_token const& token) noexcept
{
    assert(false); // TODO: not implemented, but this can't throw
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
