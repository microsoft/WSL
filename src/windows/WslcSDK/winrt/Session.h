/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Session.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Session class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Session.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Session : SessionT<Session>
{
    Session() = default;
    Session(WslcSession session);

    static winrt::Microsoft::WSL::Containers::Session Create(winrt::Microsoft::WSL::Containers::SessionSettings const& settings);
    void Terminate();
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> PullImageAsync(
        winrt::Microsoft::WSL::Containers::PullImageOptions options);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> ImportImageAsync(hstring path, hstring imageName);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> LoadImageAsync(hstring path);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> PushImageAsync(
        winrt::Microsoft::WSL::Containers::PushImageOptions options);
    void DeleteImage(hstring const& nameOrId);
    void TagImage(winrt::Microsoft::WSL::Containers::TagImageOptions const& options);
    void CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdRequirements const& options);
    void DeleteVhdVolume(hstring const& name);
    hstring Authenticate(winrt::Windows::Foundation::Uri const& serverAddress, hstring const& username, hstring const& password);
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::ImageInfo> Images();

    WslcSession ToHandle();
    WslcSession* ToHandlePointer();

private:
    wil::unique_any<WslcSession, decltype(&WslcReleaseSession), &WslcReleaseSession> m_session;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct Session : SessionT<Session, implementation::Session>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(Session);
