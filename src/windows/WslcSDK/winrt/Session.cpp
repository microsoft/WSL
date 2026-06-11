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
        try
        {
            auto progress = winrt::make<implementation::ImageProgress>(progressMessage);
            ProgressCallbackHelper<decltype(progress)>::ReportProgress(context, progress);
        }
        CATCH_LOG();
        return S_OK;
    }

} // namespace

Session::Session(winrt::Microsoft::WSL::Containers::SessionSettings const& settings) : m_settings(settings)
{
    if (!m_settings)
    {
        throw winrt::hresult_error(E_POINTER, L"Session settings cannot be null");
    }
}

void Session::Start()
{
    if (m_session)
    {
        throw winrt::hresult_illegal_method_call(L"Session has already been started");
    }

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateSession(GetStructPointer(m_settings), m_session.put(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
    m_settings = nullptr;

    winrt::check_hresult(WslcGetSessionTerminationEvent(m_session.get(), m_terminationEvent.put()));

    m_terminationWait.reset(CreateThreadpoolWait(&Session::OnTerminated, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_terminationWait);
    SetThreadpoolWait(m_terminationWait.get(), m_terminationEvent.get(), nullptr);
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
        throw winrt::hresult_error(E_POINTER, L"Container settings cannot be null");
    }

    return winrt::make<implementation::Container>(ToHandle(), containerSettings);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::PullImageAsync(winrt::Microsoft::WSL::Containers::PullImageOptions options)
{
    if (!options)
    {
        throw winrt::hresult_error(E_POINTER, L"Options for pull cannot be null");
    }

    EnsureStarted();

    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    auto pullOptions = GetStruct(options);
    pullOptions.progressCallback = ImageProgressCallback;
    pullOptions.progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcPullSessionImage(ToHandle(), &pullOptions, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

IAsyncActionWithProgress<winrt::Microsoft::WSL::Containers::ImageProgress> Session::ImportImageAsync(hstring path, hstring imageName)
{
    if (path.empty())
    {
        throw winrt::hresult_invalid_argument(L"Path cannot be empty");
    }

    if (imageName.empty())
    {
        throw winrt::hresult_invalid_argument(L"Image name cannot be empty");
    }

    EnsureStarted();

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
        throw winrt::hresult_invalid_argument(L"Path cannot be empty");
    }

    EnsureStarted();

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
    if (!options)
    {
        throw winrt::hresult_error(E_POINTER, L"Options for push cannot be null");
    }

    EnsureStarted();

    auto self = get_strong(); // keep session alive across suspension
    co_await winrt::resume_background();

    auto context = ProgressCallbackHelper<winrt::Microsoft::WSL::Containers::ImageProgress>{co_await winrt::get_progress_token()};

    auto pushOptions = GetStruct(options);
    pushOptions.progressCallback = ImageProgressCallback;
    pushOptions.progressCallbackContext = &context;

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcPushSessionImage(ToHandle(), &pushOptions, errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::DeleteImage(hstring const& nameOrId)
{
    if (nameOrId.empty())
    {
        throw winrt::hresult_invalid_argument(L"Image name cannot be empty");
    }

    EnsureStarted();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDeleteSessionImage(ToHandle(), winrt::to_string(nameOrId).c_str(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::TagImage(winrt::Microsoft::WSL::Containers::TagImageOptions const& options)
{
    if (!options)
    {
        throw winrt::hresult_error(E_POINTER, L"Tag image options cannot be null");
    }

    EnsureStarted();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcTagSessionImage(ToHandle(), GetStructPointer(options), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::CreateVhdVolume(winrt::Microsoft::WSL::Containers::VhdOptions const& options)
{
    if (!options)
    {
        throw winrt::hresult_error(E_POINTER, L"VHD options cannot be null");
    }

    EnsureStarted();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateSessionVhdVolume(ToHandle(), GetStructPointer(options), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

void Session::DeleteVhdVolume(hstring const& name)
{
    if (name.empty())
    {
        throw winrt::hresult_invalid_argument(L"VHD name cannot be empty");
    }

    EnsureStarted();

    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcDeleteSessionVhdVolume(ToHandle(), winrt::to_string(name).c_str(), errorMessage.put());
    THROW_MSG_IF_FAILED(hr, errorMessage);
}

hstring Session::Authenticate(Uri const& serverAddress, hstring const& username, hstring const& password)
{
    if (!serverAddress)
    {
        throw winrt::hresult_invalid_argument(L"Server address cannot be null");
    }

    if (username.empty())
    {
        throw winrt::hresult_invalid_argument(L"Username cannot be empty");
    }

    EnsureStarted();

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
    EnsureStarted();

    wil::unique_cotaskmem_array_ptr<WslcImageInfo> imagesArray;
    winrt::check_hresult(WslcListSessionImages(ToHandle(), imagesArray.put(), imagesArray.size_address<uint32_t>()));

    auto images = std::vector<winrt::Microsoft::WSL::Containers::ImageInfo>();
    images.reserve(imagesArray.size());
    for (uint32_t i = 0; i < imagesArray.size(); i++)
    {
        images.push_back(winrt::make<implementation::ImageInfo>(imagesArray[i]));
    }

    return winrt::single_threaded_vector(std::move(images)).GetView();
}

WslcSession Session::ToHandle()
{
    EnsureStarted();
    return m_session.get();
}

void CALLBACK Session::OnTerminated(PTP_CALLBACK_INSTANCE /* instance */, PVOID context, PTP_WAIT /* wait */, TP_WAIT_RESULT /* waitResult */) noexcept
{
    try
    {
        auto session = static_cast<Session*>(context);

        WslcSessionTerminationReason reason = WSLC_SESSION_TERMINATION_REASON_UNKNOWN;
        LOG_IF_FAILED(WslcGetSessionTerminationReason(session->m_session.get(), &reason));

        session->m_terminatedEvent(static_cast<SessionTerminationReason>(reason));
    }
    CATCH_LOG();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
