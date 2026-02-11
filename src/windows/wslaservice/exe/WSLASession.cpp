/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.cpp

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "precomp.h"
#include "WSLASession.h"
#include "WSLAContainer.h"
#include "ServiceProcessLauncher.h"
#include "WslCoreFilesystem.h"

using namespace wsl::windows::common;
using relay::MultiHandleWait;
using wsl::windows::service::wsla::WSLAVirtualMachine;

constexpr auto c_containerdStorage = "/var/lib/docker";

namespace {

std::pair<std::string, std::optional<std::string>> ParseImage(const std::string& Input)
{
    size_t separator = Input.find_last_of(':');
    if (separator == std::string::npos)
    {
        return {Input, {}};
    }

    THROW_HR_IF_MSG(E_INVALIDARG, separator >= Input.size() - 1 || separator == 0, "Invalid image: %hs", Input.c_str());

    return {Input.substr(0, separator), Input.substr(separator + 1)};
}

bool IsContainerNameValid(LPCSTR Name)
{
    size_t length = 0;
    const auto& locale = std::locale::classic();
    while (*Name != '\0')
    {
        if (!std::isalnum(*Name, locale) && *Name != '_' && *Name != '-' && *Name != '.')
        {
            return false;
        }

        Name++;
        length++;
    }

    return length > 0 && length <= WSLA_MAX_CONTAINER_NAME_LENGTH;
}

} // namespace

namespace wsl::windows::service::wsla {

HRESULT WSLASession::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    RETURN_HR_IF(E_POINTER, ProcessHandle == nullptr);

    wil::unique_handle process{OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, GetCurrentProcessId())};
    THROW_LAST_ERROR_IF(!process);

    *ProcessHandle = process.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::Initialize(_In_ const WSLA_SESSION_INIT_SETTINGS* Settings, _In_ IWSLAVirtualMachine* Vm)
try
{
    RETURN_HR_IF(E_POINTER, Settings == nullptr || Vm == nullptr);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_virtualMachine.has_value());

    m_id = Settings->SessionId;
    m_displayName = Settings->DisplayName ? Settings->DisplayName : L"";
    m_featureFlags = Settings->FeatureFlags;

    // Get user token for the current process
    const auto tokenInfo = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());

    WSL_LOG(
        "SessionInitialized",
        TraceLoggingValue(m_id, "SessionId"),
        TraceLoggingValue(m_displayName.c_str(), "DisplayName"),
        TraceLoggingValue(Settings->CreatorPid, "CreatorPid"));

    // Create the VM.
    m_virtualMachine.emplace(Vm, Settings);

    // Make sure that everything is destroyed correctly if an exception is thrown.
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_FAILED(Terminate()); });

    // Configure storage.
    ConfigureStorage(*Settings, tokenInfo->User.Sid);

    // Launch dockerd
    StartDockerd();

    // Wait for dockerd to be ready before starting the event tracker.
    THROW_WIN32_IF_MSG(
        ERROR_TIMEOUT, !m_containerdReadyEvent.wait(Settings->BootTimeoutMs), "Timed out waiting for dockerd to start");

    auto [_, __, channel] = m_virtualMachine->Fork(WSLA_FORK::Thread);

    m_dockerClient.emplace(std::move(channel), m_virtualMachine->TerminatingEvent(), m_virtualMachine->VmId(), 10 * 1000);

    //  Start the event tracker.
    m_eventTracker.emplace(m_dockerClient.value(), m_id, m_ioRelay);

    // Recover any existing containers from storage.
    RecoverExistingContainers();

    errorCleanup.release();
    return S_OK;
}
CATCH_RETURN()

WSLASession::~WSLASession()
{
    WSL_LOG("SessionTerminated", TraceLoggingValue(m_id, "SessionId"), TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    LOG_IF_FAILED(Terminate());

    if (m_destructionCallback)
    {
        m_destructionCallback();
    }
}

void WSLASession::SetDestructionCallback(std::function<void()>&& callback)
{
    m_destructionCallback = std::move(callback);
}

void WSLASession::ConfigureStorage(const WSLA_SESSION_INIT_SETTINGS& Settings, PSID UserSid)
{
    if (Settings.StoragePath == nullptr)
    {
        // If no storage path is specified, use a tmpfs for convenience.
        m_virtualMachine->Mount("", c_containerdStorage, "tmpfs", "", 0);
        return;
    }

    std::filesystem::path storagePath{Settings.StoragePath};
    THROW_HR_IF_MSG(E_INVALIDARG, !storagePath.is_absolute(), "Storage path is not absolute: %ls", storagePath.c_str());

    m_storageVhdPath = storagePath / "storage.vhdx";

    std::string diskDevice;
    std::optional<ULONG> diskLun{};
    bool vhdCreated = false;

    auto deleteVhdOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (vhdCreated)
        {
            if (diskLun.has_value())
            {
                m_virtualMachine->DetachDisk(diskLun.value());
            }

            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_storageVhdPath.c_str()));
        }
    });

    auto result =
        wil::ResultFromException([&]() { diskDevice = m_virtualMachine->AttachDisk(m_storageVhdPath.c_str(), false).second; });

    if (FAILED(result))
    {
        THROW_HR_IF_MSG(
            result,
            result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            "Failed to attach vhd: %ls",
            m_storageVhdPath.c_str());

        // If the VHD wasn't found, create it.
        WSL_LOG("CreateStorageVhd", TraceLoggingValue(m_storageVhdPath.c_str(), "StorageVhdPath"));

        std::filesystem::create_directories(storagePath);
        wsl::core::filesystem::CreateVhd(m_storageVhdPath.c_str(), Settings.MaximumStorageSizeMb * _1MB, UserSid, false, false);
        vhdCreated = true;

        // Then attach the new disk.
        std::tie(diskLun, diskDevice) = m_virtualMachine->AttachDisk(m_storageVhdPath.c_str(), false);

        // Then format it.
        Ext4Format(diskDevice);
    }

    // Mount the device to /root.
    m_virtualMachine->Mount(diskDevice.c_str(), c_containerdStorage, "ext4", "", 0);

    deleteVhdOnFailure.release();
}

HRESULT WSLASession::GetId(ULONG* Id)
{
    *Id = m_id;

    return S_OK;
}

void WSLASession::OnDockerdExited()
{
    if (!m_sessionTerminatingEvent.is_signaled())
    {
        WSL_LOG("UnexpectedDockerdExit", TraceLoggingValue(m_displayName.c_str(), "Name"));
    }
}

void WSLASession::OnDockerdLog(const gsl::span<char>& buffer)
try
{
    if (buffer.empty())
    {
        return;
    }

    constexpr auto c_containerdReadyLogLine = "API listen on /var/run/docker.sock";

    std::string entry = {buffer.begin(), buffer.end()};
    WSL_LOG("ContainerdLog", TraceLoggingValue(entry.c_str(), "Content"), TraceLoggingValue(m_displayName.c_str(), "Name"));

    if (!m_containerdReadyEvent.is_signaled())
    {
        if (entry.find(c_containerdReadyLogLine) != std::string::npos)
        {
            m_containerdReadyEvent.SetEvent();
        }
    }
}
CATCH_LOG();

void WSLASession::StartDockerd()
{
    std::vector<std::string> args{{"/usr/bin/dockerd"}};

    if (WI_IsFlagSet(m_featureFlags, WslaFeatureFlagsDebug))
    {
        args.emplace_back("--debug");
    }

    ServiceProcessLauncher launcher{"/usr/bin/dockerd", args, {{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"}}};

    m_dockerdProcess = launcher.Launch(*m_virtualMachine);

    // Read stdout & stderr.
    m_ioRelay.AddHandle(std::make_unique<windows::common::relay::LineBasedReadHandle>(
        m_dockerdProcess->GetStdHandle(1), [&](const auto& data) { OnDockerdLog(data); }, false));

    m_ioRelay.AddHandle(std::make_unique<windows::common::relay::LineBasedReadHandle>(
        m_dockerdProcess->GetStdHandle(2), [&](const auto& data) { OnDockerdLog(data); }, false));

    // Monitor dockerd's exist so we can detect abnormal exits.
    m_ioRelay.AddHandle(std::make_unique<windows::common::relay::EventHandle>(
        m_dockerdProcess->GetExitEvent(), std::bind(&WSLASession::OnDockerdExited, this)));
}

HRESULT WSLASession::PullImage(
    LPCSTR ImageUri,
    const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation,
    IProgressCallback* ProgressCallback,
    WSLA_ERROR_INFO* Error)
try
{
    UNREFERENCED_PARAMETER(RegistryAuthenticationInformation);

    RETURN_HR_IF_NULL(E_POINTER, ImageUri);

    auto [repo, tag] = ParseImage(ImageUri);

    std::lock_guard lock{m_lock};

    auto requestContext = m_dockerClient->PullImage(repo, tag);

    relay::MultiHandleWait io;

    std::optional<boost::beast::http::status> pullResult;

    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG("PullHttpResponse", TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        pullResult = response.result();
    };

    std::string errorJson;
    auto onChunk = [&](const gsl::span<char>& Content) {
        if (pullResult.has_value() && pullResult.value() != boost::beast::http::status::ok)
        {
            // If the status code is an error, then this is an error message, not a progress update.
            errorJson.append(Content.data(), Content.size());
            return;
        }

        std::string contentString{Content.begin(), Content.end()};
        WSL_LOG("ImagePullProgress", TraceLoggingValue(ImageUri, "Image"), TraceLoggingValue(contentString.c_str(), "Content"));

        if (ProgressCallback == nullptr)
        {
            return;
        }

        auto parsed = wsl::shared::FromJson<docker_schema::CreateImageProgress>(contentString.c_str());

        THROW_IF_FAILED(ProgressCallback->OnProgress(
            parsed.status.c_str(), parsed.id.c_str(), parsed.progressDetail.current, parsed.progressDetail.total));
    };

    auto onCompleted = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));
    io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));
    io.AddHandle(std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(
        *requestContext, std::move(onHttpResponse), std::move(onChunk), std::move(onCompleted)));

    io.Run({});

    THROW_HR_IF(E_ABORT, m_sessionTerminatingEvent.is_signaled());
    THROW_HR_IF(E_UNEXPECTED, !pullResult.has_value());

    if (pullResult.value() != boost::beast::http::status::ok)
    {
        std::string errorMessage;
        if (static_cast<int>(pullResult.value()) >= 400 && static_cast<int>(pullResult.value()) < 500)
        {
            // pull failed, parse the error message.
            errorMessage = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str()).message;
            if (Error != nullptr)
            {
                Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(errorMessage.c_str()).release();
            }
        }

        if (pullResult.value() == boost::beast::http::status::not_found)
        {
            THROW_HR_MSG(WSLA_E_IMAGE_NOT_FOUND, "%hs", errorMessage.c_str());
        }
        else if (pullResult.value() == boost::beast::http::status::bad_request)
        {
            THROW_HR_MSG(E_INVALIDARG, "%hs", errorMessage.c_str());
        }
        else
        {
            THROW_HR_MSG(E_FAIL, "%hs", errorMessage.c_str());
        }
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::LoadImage(ULONG ImageHandle, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto requestContext = m_dockerClient->LoadImage(ContentSize);

    ImportImageImpl(*requestContext, ImageHandle);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ImportImage(ULONG ImageHandle, LPCSTR ImageName, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);
    RETURN_HR_IF_NULL(E_POINTER, ImageName);

    auto [repo, tag] = ParseImage(ImageName);

    THROW_HR_IF_MSG(E_INVALIDARG, !tag.has_value(), "Expected tag for image import: %hs", ImageName);

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto requestContext = m_dockerClient->ImportImage(repo, tag.value(), ContentSize);

    ImportImageImpl(*requestContext, ImageHandle);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, ULONG InputHandle)
{
    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(InputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    relay::MultiHandleWait io;

    std::optional<boost::beast::http::status> importResult;

    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG("ImageImportHttpResponse", TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        importResult = response.result();
    };

    std::string errorJson;
    auto onProgress = [&](const gsl::span<char>& buffer) {
        WI_ASSERT(importResult.has_value());

        if (importResult.value() != boost::beast::http::status::ok)
        {
            // If the import failed, accumulate the error message.
            errorJson.append(buffer.data(), buffer.size());
        }
        else
        {
            // TODO: report progress to caller.
            std::string entry = {buffer.begin(), buffer.end()};
            WSL_LOG("ImageImportProgress", TraceLoggingValue(entry.c_str(), "Content"));
        }
    };

    auto onCompleted = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<relay::RelayHandle<relay::ReadHandle>>(
        common::relay::HandleWrapper{std::move(imageFileHandle)}, common::relay::HandleWrapper{Request.stream.native_handle()}));

    io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));

    io.AddHandle(std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(
        Request, std::move(onHttpResponse), std::move(onProgress), std::move(onCompleted)));

    io.Run({});

    THROW_HR_IF(E_UNEXPECTED, !importResult.has_value());

    if (importResult.value() != boost::beast::http::status::ok)
    {
        // Import failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());

        // TODO: Return error message to client.
        THROW_HR_MSG(E_FAIL, "Image import failed: %hs", error.message.c_str());
    }
}

HRESULT WSLASession::ExportContainer(ULONG OutHandle, LPCSTR ContainerID, IProgressCallback* ProgressCallback, WSLA_ERROR_INFO* Error)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);
    RETURN_HR_IF_NULL(E_POINTER, ContainerID);
    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->ExportContainer(ContainerID);
    ExportContainerImpl(retVal, OutHandle, Error);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::ExportContainerImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, ULONG OutputHandle, WSLA_ERROR_INFO* Error)
{
    wil::unique_handle containerFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    relay::MultiHandleWait io;

    auto onCompleted = [&]() {
        io.Cancel();
        WSL_LOG("OnCompletedCalledForExport", TraceLoggingValue("OnCompletedCalledForExport", "Content"));
    };

    std::string errorJson;
    auto accumulateError = [&](const gsl::span<char>& buffer) {
        // If the export failed, accumulate the error message.
        errorJson.append(buffer.data(), buffer.size());
    };

    if (SocketCodePair.first != 200)
    {
        io.AddHandle(std::make_unique<relay::ReadHandle>(common::relay::HandleWrapper{std::move(SocketCodePair.second)}, std::move(accumulateError)));
    }
    else
    {
        io.AddHandle(std::make_unique<relay::RelayHandle<relay::HTTPChunkBasedReadHandle>>(
            common::relay::HandleWrapper{std::move(SocketCodePair.second)},
            common::relay::HandleWrapper{std::move(containerFileHandle), std::move(onCompleted)}));
        io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));
    }

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Export failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());
        if (Error != nullptr)
        {
            Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(error.message.c_str()).release();
        }

        if (SocketCodePair.first == 404)
        {
            THROW_HR_MSG(WSLA_E_CONTAINER_NOT_FOUND, "%hs", error.message.c_str());
        }

        THROW_HR_MSG(E_FAIL, "Container export failed: %hs", error.message.c_str());
    }
}

HRESULT WSLASession::SaveImage(ULONG OutHandle, LPCSTR ImageNameOrID, IProgressCallback* ProgressCallback, WSLA_ERROR_INFO* Error)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);
    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrID);
    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->SaveImage(ImageNameOrID);
    SaveImageImpl(retVal, OutHandle, Error);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, ULONG OutputHandle, WSLA_ERROR_INFO* Error)
{
    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    relay::MultiHandleWait io;

    auto onCompleted = [&]() { io.Cancel(); };
    std::string errorJson;
    auto accumulateError = [&](const gsl::span<char>& buffer) {
        // If the save failed, accumulate the error message.
        errorJson.append(buffer.data(), buffer.size());
    };

    if (SocketCodePair.first != 200)
    {
        io.AddHandle(std::make_unique<relay::ReadHandle>(common::relay::HandleWrapper{std::move(SocketCodePair.second)}, std::move(accumulateError)));
    }
    else
    {
        io.AddHandle(std::make_unique<relay::RelayHandle<relay::HTTPChunkBasedReadHandle>>(
            common::relay::HandleWrapper{std::move(SocketCodePair.second)},
            common::relay::HandleWrapper{std::move(imageFileHandle), std::move(onCompleted)}));
        io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));
    }

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Save failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());
        if (Error != nullptr)
        {
            Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(error.message.c_str()).release();
        }

        THROW_HR_MSG(E_FAIL, "Image save failed: %hs", error.message.c_str());
    }
}

HRESULT WSLASession::ListImages(WSLA_IMAGE_INFORMATION** Images, ULONG* Count)
try
{
    *Count = 0;
    *Images = nullptr;

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto images = m_dockerClient->ListImages();

    // Compute the number of entries.
    auto entries = std::accumulate<decltype(images.begin()), size_t>(
        images.begin(), images.end(), 0, [](auto sum, const auto& e) { return sum + e.RepoTags.size(); });

    auto output = wil::make_unique_cotaskmem<WSLA_IMAGE_INFORMATION[]>(entries);

    size_t index = 0;
    for (const auto& e : images)
    {
        // TODO: download_timestamp
        for (const auto& tag : e.RepoTags)
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, tag.c_str()) != 0);
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Hash, e.Id.c_str()) != 0);
            output[index].Size = e.Size;
            index++;
        }
    }

    WI_ASSERT(index == entries);

    *Count = static_cast<ULONG>(entries);
    *Images = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::DeleteImage(const WSLA_DELETE_IMAGE_OPTIONS* Options, WSLA_DELETED_IMAGE_INFORMATION** DeletedImages, ULONG* Count, WSLA_ERROR_INFO* Error)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF_NULL(E_POINTER, DeletedImages);
    RETURN_HR_IF_NULL(E_POINTER, Count);

    *DeletedImages = nullptr;
    *Count = 0;

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    std::vector<docker_schema::DeletedImage> deletedImages;
    try
    {
        deletedImages = m_dockerClient->DeleteImage(Options->Image, !!Options->Force, !!Options->NoPrune);
    }
    catch (const DockerHTTPException& e)
    {
        std::string errorMessage;
        if ((e.StatusCode() >= 400 && e.StatusCode() < 500))
        {
            errorMessage = e.DockerMessage<docker_schema::ErrorResponse>().message;
            if (Error != nullptr)
            {
                Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(errorMessage.c_str()).release();
            }
        }

        if (e.StatusCode() == 404)
        {
            THROW_HR_MSG(WSLA_E_IMAGE_NOT_FOUND, "%hs", errorMessage.c_str());
        }
        else if (e.StatusCode() == 409)
        {
            THROW_WIN32_MSG(ERROR_SHARING_VIOLATION, "%hs", errorMessage.c_str());
        }
        else
        {
            THROW_HR_MSG(E_FAIL, "%hs", errorMessage.c_str());
        }
    }

    THROW_HR_IF_MSG(E_FAIL, deletedImages.empty(), "Failed to delete image: %hs", Options->Image);

    auto output = wil::make_unique_cotaskmem<WSLA_DELETED_IMAGE_INFORMATION[]>(deletedImages.size());

    size_t index = 0;
    for (const auto& image : deletedImages)
    {
        THROW_HR_IF(E_UNEXPECTED, (image.Deleted.empty() && image.Untagged.empty()) || (!image.Deleted.empty() && !image.Untagged.empty()));

        if (!image.Deleted.empty())
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Deleted.c_str()) != 0);
            output[index].Type = WSLADeletedImageTypeDeleted;
        }
        else
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Untagged.c_str()) != 0);
            output[index].Type = WSLADeletedImageTypeUntagged;
        }

        index++;
    }

    *Count = static_cast<ULONG>(deletedImages.size());
    *DeletedImages = output.release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::TagImage(const WSLA_TAG_IMAGE_OPTIONS* Options, WSLA_ERROR_INFO* Error)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF_NULL(E_POINTER, Options->Repo);
    RETURN_HR_IF_NULL(E_POINTER, Options->Tag);

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    try
    {
        m_dockerClient->TagImage(Options->Image, Options->Repo, Options->Tag);
    }
    catch (const DockerHTTPException& e)
    {
        std::string errorMessage;
        if ((e.StatusCode() >= 400 && e.StatusCode() < 500))
        {
            errorMessage = e.DockerMessage<docker_schema::ErrorResponse>().message;
            if (Error != nullptr)
            {
                Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(errorMessage.c_str()).release();
            }
        }

        if (e.StatusCode() == 404)
        {
            THROW_HR_MSG(WSLA_E_IMAGE_NOT_FOUND, "%hs", errorMessage.c_str());
        }
        else if (e.StatusCode() == 400)
        {
            THROW_WIN32_MSG(ERROR_BAD_ARGUMENTS, "%hs", errorMessage.c_str());
        }
        else if (e.StatusCode() == 409)
        {
            THROW_WIN32_MSG(ERROR_SHARING_VIOLATION, "%hs", errorMessage.c_str());
        }
        else
        {
            THROW_HR_MSG(E_FAIL, "%hs", errorMessage.c_str());
        }
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateContainer(const WSLA_CONTAINER_OPTIONS* containerOptions, IWSLAContainer** Container, WSLA_ERROR_INFO* Error)
try
{
    RETURN_HR_IF_NULL(E_POINTER, containerOptions);

    // Validate that Image is not null.
    RETURN_HR_IF(E_INVALIDARG, containerOptions->Image == nullptr);

    std::lock_guard lock{m_lock};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Validate that name & images are valid.
    RETURN_HR_IF_MSG(
        E_INVALIDARG,
        containerOptions->Name != nullptr && !IsContainerNameValid(containerOptions->Name),
        "Invalid container name: %hs",
        containerOptions->Name);

    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);

    // TODO: Log entrance into the function.

    try
    {
        auto& it = m_containers.emplace_back(WSLAContainerImpl::Create(
            *containerOptions,
            *m_virtualMachine,
            std::bind(&WSLASession::OnContainerDeleted, this, std::placeholders::_1),
            m_eventTracker.value(),
            m_dockerClient.value(),
            m_ioRelay));

        THROW_IF_FAILED(it->ComWrapper().QueryInterface(__uuidof(IWSLAContainer), (void**)Container));

        return S_OK;
    }
    catch (const DockerHTTPException& e)
    {
        std::string errorMessage;
        if ((e.StatusCode() >= 400 && e.StatusCode() < 500))
        {
            errorMessage = e.DockerMessage<docker_schema::ErrorResponse>().message;
        }

        if (Error != nullptr)
        {
            Error->UserErrorMessage = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(errorMessage.c_str()).release();
        }

        if (e.StatusCode() == 404)
        {
            THROW_HR_MSG(WSLA_E_IMAGE_NOT_FOUND, "%hs", errorMessage.c_str());
        }
        else if (e.StatusCode() == 409)
        {
            THROW_WIN32_MSG(ERROR_ALREADY_EXISTS, "%hs", errorMessage.c_str());
        }

        return E_FAIL;
    }
}
CATCH_RETURN();

HRESULT WSLASession::OpenContainer(LPCSTR Id, IWSLAContainer** Container)
try
{
    THROW_HR_IF_MSG(E_INVALIDARG, !IsContainerNameValid(Id), "Invalid container name: %hs", Id);

    // Look for an exact ID match first.
    std::lock_guard lock{m_lock};
    auto it = std::ranges::find_if(m_containers, [Id](const auto& e) { return e->ID() == Id; });

    // If no match is found, call Inspect() so that partial IDs and names are matched.
    if (it == m_containers.end())
    {
        // TODO: consider a trimmed down version of inspect to avoid parsing the full response.
        docker_schema::InspectContainer inspectResult;

        try
        {
            inspectResult = wsl::shared::FromJson<docker_schema::InspectContainer>(m_dockerClient->InspectContainer(Id).c_str());
        }
        catch (DockerHTTPException& e)
        {
            RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), e.StatusCode() == 404, "Container not found: '%hs'", Id);
            RETURN_HR_IF_MSG(WSLA_E_CONTAINER_PREFIX_AMBIGUOUS, e.StatusCode() == 400, "Ambiguous prefix: '%hs'", Id);

            THROW_HR_MSG(E_FAIL, "Unexpected error inspecting container '%hs': %hs", Id, e.what());
        }

        it = std::ranges::find_if(m_containers, [&](const auto& e) { return e->ID() == inspectResult.Id; });
        RETURN_HR_IF_MSG(
            E_UNEXPECTED, it == m_containers.end(), "Resolved container ID (%hs -> %hs) not found", Id, inspectResult.Id.c_str());
    }

    THROW_IF_FAILED((*it)->ComWrapper().QueryInterface(__uuidof(IWSLAContainer), (void**)Container));
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ListContainers(WSLA_CONTAINER** Containers, ULONG* Count)
try
{
    *Count = 0;
    *Containers = nullptr;

    std::lock_guard lock{m_lock};

    auto output = wil::make_unique_cotaskmem<WSLA_CONTAINER[]>(m_containers.size());

    size_t index = 0;
    for (const auto& e : m_containers)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, e->Image().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Name, e->Name().c_str()) != 0);
        e->GetState(&output[index].State);
        index++;
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateRootNamespaceProcess(LPCSTR Executable, const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    if (Errno != nullptr)
    {
        *Errno = -1; // Make sure not to return 0 if something fails.
    }

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    auto process = m_virtualMachine->CreateLinuxProcess(Executable, *Options, Errno);
    THROW_IF_FAILED(process.CopyTo(Process));

    return S_OK;
}
CATCH_RETURN();

void WSLASession::Ext4Format(const std::string& Device)
{
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, Device});
    auto result = launcher.Launch(*m_virtualMachine).WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());
}

HRESULT WSLASession::FormatVirtualDisk(LPCWSTR Path)
try
{
    THROW_HR_IF_MSG(E_INVALIDARG, !std::filesystem::path(Path).is_absolute(), "FormatVirtualDisk called with a relative path: %ls", Path);

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Attach the disk to the VM (AttachDisk() performs the access check for the VHD file).
    auto [lun, device] = m_virtualMachine->AttachDisk(Path, false);

    // N.B. DetachDisk calls sync() before detaching.
    auto detachDisk = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this, lun]() { m_virtualMachine->DetachDisk(lun); });

    // Format it to ext4.
    Ext4Format(device);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::Terminate()
try
{
    // m_sessionTerminatingEvent is always valid, so it can be signalled with the lock.
    // This allows a session to be unblocked if a stuck operation is holding the lock.
    m_sessionTerminatingEvent.SetEvent();

    std::lock_guard lock{m_lock};

    // This will delete all containers. Needs to be done before the VM is terminated.
    m_containers.clear();

    // Stop the IO relay.
    // This stops:
    // - container state monitoring.
    // - container init process relays
    // - execs relays
    // - container logs relays
    m_ioRelay.Stop();

    m_eventTracker.reset();
    m_dockerClient.reset();

    // Stop dockerd.
    // N.B. dockerd wait a couple seconds if there are any outstanding HTTP request sockets opened.
    if (m_dockerdProcess.has_value())
    {
        LOG_IF_FAILED(m_dockerdProcess->Get().Signal(WSLASignalSIGTERM));

        int exitCode = -1;
        try
        {
            exitCode = m_dockerdProcess->Wait(30 * 1000);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            m_dockerdProcess->Get().Signal(WSLASignalSIGKILL);
            exitCode = m_dockerdProcess->Wait(10 * 1000);
        }

        WSL_LOG("DockerdExit", TraceLoggingValue(exitCode, "code"));
    }

    if (m_virtualMachine)
    {
        // N.B. dockerd has exited by this point, so unmounting the VHD is safe since no container can be running.
        try
        {
            m_virtualMachine->Unmount(c_containerdStorage);
        }
        CATCH_LOG();

        m_virtualMachine.reset();
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::MountWindowsFolder(LPCWSTR WindowsPath, LPCSTR LinuxPath, BOOL ReadOnly)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}
CATCH_RETURN();

HRESULT WSLASession::UnmountWindowsFolder(LPCSTR LinuxPath)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->UnmountWindowsFolder(LinuxPath);
}
CATCH_RETURN();

HRESULT WSLASession::MapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->MapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::UnmapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->UnmapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::OnContainerDeleted(const WSLAContainerImpl* Container)
{
    std::lock_guard lock{m_lock};
    WI_VERIFY(std::erase_if(m_containers, [Container](const auto& e) { return e.get() == Container; }) == 1);
}

HRESULT WSLASession::GetState(_Out_ WSLASessionState* State)
{
    std::lock_guard lock{m_lock};
    *State = m_virtualMachine ? WSLASessionStateRunning : WSLASessionStateTerminated;
    return S_OK;
}

void WSLASession::RecoverExistingContainers()
{
    WI_ASSERT(m_dockerClient.has_value());
    WI_ASSERT(m_eventTracker.has_value());
    WI_ASSERT(m_virtualMachine.has_value());

    auto containers = m_dockerClient->ListContainers(true); // all=true to include stopped containers

    for (const auto& dockerContainer : containers)
    {
        try
        {
            auto container = WSLAContainerImpl::Open(
                dockerContainer,
                *m_virtualMachine,
                std::bind(&WSLASession::OnContainerDeleted, this, std::placeholders::_1),
                m_eventTracker.value(),
                m_dockerClient.value(),
                m_ioRelay);

            m_containers.emplace_back(std::move(container));
        }
        catch (...)
        {
            // Log but don't fail the session startup if a single container fails to recover.
            LOG_CAUGHT_EXCEPTION_MSG("Failed to recover container: %hs", dockerContainer.Id.c_str());
        }
    }

    WSL_LOG(
        "ContainersRecovered",
        TraceLoggingValue(m_displayName.c_str(), "SessionName"),
        TraceLoggingValue(m_containers.size(), "ContainerCount"));
}

} // namespace wsl::windows::service::wsla
