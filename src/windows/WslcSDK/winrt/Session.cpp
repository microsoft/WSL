/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Session.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Session class.

--*/

#include "precomp.h"
#include "Session.h"
#include "SessionSettings.h"
#include "Microsoft.WSL.Containers.Session.g.cpp"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::WSL::Containers::implementation {

namespace {

    HRESULT CALLBACK ImageProgressCallback(const WslcImageProgressMessage* progressMessage, PVOID context) noexcept
    {
        auto progress = winrt::make<implementation::ImageProgress>(progressMessage);
        ProgressCallbackHelper<decltype(progress)>::ReportProgress(context, progress);
        return S_OK;
    }

} // namespace

Session::Session(winrt::Microsoft::WSL::Containers::SessionSettings const& settings) : m_settings(settings)
{
    if (!m_settings)
    {
        throw winrt::hresult_invalid_argument(L"Session settings cannot be null");
    }
}

void Session::Start()
{
    if (m_session)
    {
        throw winrt::hresult_illegal_method_call(L"Session has already been started");
    }

    winrt::check_hresult(WslcSetSessionSettingsTerminationCallback(GetStructPointer(m_settings), TerminatedCallback, /* context */ this));

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateSession(GetStructPointer(m_settings), m_session.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    m_settings = nullptr;

    // This object needs to stay alive for as long as the callbacks may be invoked even if all other references to it are dropped.
    // We increase its ref count here, and decrease it once the session terminates.
    AddRef();
}

void Session::EnsureStarted() const
{
    if (!m_session)
    {
        throw winrt::hresult_illegal_method_call(L"Session has not been started");
    }
}

void Session::Terminate()
{
    winrt::check_hresult(WslcTerminateSession(ToHandle()));
}

winrt::Microsoft::WSL::Containers::Container Session::CreateContainer(winrt::Microsoft::WSL::Containers::ContainerSettings const& containerSettings)
{
    EnsureStarted();

    if (!containerSettings)
    {
        throw winrt::hresult_error(E_POINTER);
    }

    return winrt::make<implementation::Container>(ToHandle(), containerSettings);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PullImageAsync(winrt::Microsoft::WSL::Containers::PullImageOptions options)
{
    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    auto uri = winrt::to_string(options.Uri());
    auto auth = winrt::to_string(options.RegistryAuth());

    WslcPullImageOptions pullOptions{};
    pullOptions.uri = uri.c_str();
    pullOptions.registryAuth = auth.empty() ? nullptr : auth.c_str();
    pullOptions.progressCallback = ImageProgressCallback;
    pullOptions.progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcPullSessionImage(ToHandle(), &pullOptions, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::ImportImageAsync(hstring path, hstring imageName)
{
    if (path.empty() || imageName.empty())
    {
        throw winrt::hresult_invalid_argument();
    }

    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    auto name = winrt::to_string(imageName);

    WslcImportImageOptions importOptions{};
    importOptions.progressCallback = ImageProgressCallback;
    importOptions.progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcImportSessionImageFromFile(ToHandle(), name.c_str(), path.c_str(), &importOptions, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::LoadImageAsync(hstring path)
{
    if (path.empty())
    {
        throw winrt::hresult_invalid_argument();
    }

    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    WslcLoadImageOptions loadOptions{};
    loadOptions.progressCallback = ImageProgressCallback;
    loadOptions.progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcLoadSessionImageFromFile(ToHandle(), path.c_str(), &loadOptions, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PushImageAsync(winrt::Microsoft::WSL::Containers::PushImageOptions options)
{
    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    auto pushStruct = GetStructPointer(options);
    pushStruct->progressCallback = ImageProgressCallback;
    pushStruct->progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcPushSessionImage(ToHandle(), pushStruct, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
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

void Session::CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdOptions const& options)
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
    auto hr = WslcSessionAuthenticate(
        ToHandle(),
        winrt::to_string(serverAddress.ToString()).c_str(),
        winrt::to_string(username).c_str(),
        winrt::to_string(password).c_str(),
        token.put(),
        errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return winrt::to_hstring(token.get());
}

winrt::event_token Session::Terminated(winrt::Microsoft::WSL::Containers::SessionTerminationHandler const& handler)
{
    return m_terminatedEvent.add(handler);
}

void Session::Terminated(winrt::event_token const& token) noexcept
{
    m_terminatedEvent.remove(token);
}

IVectorView<winrt::Microsoft::WSL::Containers::ImageInfo> Session::Images()
{
    WslcImageInfo* imagesArrayPtr = nullptr;
    uint32_t count = 0;
    winrt::check_hresult(WslcListSessionImages(ToHandle(), &imagesArrayPtr, &count));

    // We can't pass this directly to WslcListSessionImages because the field for size is of a different type.
    wil::unique_cotaskmem_array_ptr<WslcImageInfo> imagesArray{imagesArrayPtr, count};

    auto images = winrt::single_threaded_vector<winrt::Microsoft::WSL::Containers::ImageInfo>();
    for (uint32_t i = 0; i < count; i++)
    {
        images.Append(winrt::make<implementation::ImageInfo>(imagesArray[i]));
    }

    return images.GetView();
}

WslcSession Session::ToHandle()
{
    EnsureStarted();
    return m_session.get();
}

void CALLBACK Session::TerminatedCallback(_In_ WslcSessionTerminationReason reason, _In_opt_ PVOID context)
{
    winrt::com_ptr<Session> session;

    // No other callback should be called after this event, so we no longer need to keep the object alive.
    // This takes ownership without increasing the ref count to account for the AddRef in Start().
    session.attach(static_cast<Session*>(context));

    session->m_terminatedEvent(*session, static_cast<SessionTerminationReason>(reason));
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
