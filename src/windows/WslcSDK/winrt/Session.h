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
    Session(winrt::Microsoft::WSL::Containers::SessionSettings const& settings);

    void Start();
    void Terminate();
    winrt::Microsoft::WSL::Containers::Container CreateContainer(winrt::Microsoft::WSL::Containers::ContainerSettings const& containerSettings);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> PullImageAsync(
        winrt::Microsoft::WSL::Containers::PullImageOptions options);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> ImportImageAsync(hstring path, hstring imageName);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> LoadImageAsync(hstring path);
    winrt::Windows::Foundation::IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> PushImageAsync(
        winrt::Microsoft::WSL::Containers::PushImageOptions options);
    void DeleteImage(hstring const& nameOrId);
    void TagImage(winrt::Microsoft::WSL::Containers::TagImageOptions const& options);
    void CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdOptions const& options);
    void DeleteVhdVolume(hstring const& name);
    hstring Authenticate(winrt::Windows::Foundation::Uri const& serverAddress, hstring const& username, hstring const& password);
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::WSL::Containers::ImageInfo> Images();
    winrt::event_token Terminated(winrt::Microsoft::WSL::Containers::SessionTerminationHandler const& handler);
    void Terminated(winrt::event_token const& token) noexcept;

    WslcSession ToHandle();

private:
    void EnsureStarted() const;
    winrt::Microsoft::WSL::Containers::SessionSettings m_settings; // Only kept until Start() is called

    // Threadpool callback that raises the Terminated event once the session's termination handle is signaled.
    static void CALLBACK OnTerminated(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT waitResult) noexcept;

    winrt::event<winrt::Microsoft::WSL::Containers::SessionTerminationHandler> m_terminatedEvent;
    wil::unique_any<WslcSession, decltype(&WslcReleaseSession), &WslcReleaseSession> m_session{nullptr};

    // Bridges the one-off termination event surfaced by the SDK to the WinRT Terminated event.
    wil::unique_handle m_terminationEvent;
    wil::unique_threadpool_wait m_terminationWait;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct Session : SessionT<Session, implementation::Session>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(Session);
