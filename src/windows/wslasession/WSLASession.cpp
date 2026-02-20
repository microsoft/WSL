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
using wsl::shared::Localization;
using wsl::windows::service::wsla::WSLASession;
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

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslaInvalidImage(Input), separator >= Input.size() - 1 || separator == 0);

    return {Input.substr(0, separator), Input.substr(separator + 1)};
}

void ValidateContainerName(LPCSTR Name)
{
    const auto& locale = std::locale::classic();
    size_t i = 0;

    for (; Name[i] != '\0'; i++)
    {
        if (!std::isalnum(Name[i], locale) && Name[i] != '_' && Name[i] != '-' && Name[i] != '.')
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslaInvalidContainerName(Name));
        }
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslaInvalidContainerName(Name), i == 0 || i > WSLA_MAX_CONTAINER_NAME_LENGTH);
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
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessagePathNotAbsolute(Settings.StoragePath), !storagePath.is_absolute());

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

HRESULT WSLASession::PullImage(LPCSTR ImageUri, const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(RegistryAuthenticationInformation);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageUri);

    auto [repo, tag] = ParseImage(ImageUri);

    std::lock_guard lock{m_lock};

    auto requestContext = m_dockerClient->PullImage(repo, tag);

    auto io = CreateIOContext();

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
    io.AddHandle(std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(
        *requestContext, std::move(onHttpResponse), std::move(onChunk), std::move(onCompleted)));

    io.Run({});

    THROW_HR_IF(E_UNEXPECTED, !pullResult.has_value());

    if (pullResult.value() != boost::beast::http::status::ok)
    {
        std::string errorMessage;
        if (static_cast<int>(pullResult.value()) >= 400 && static_cast<int>(pullResult.value()) < 500)
        {
            // pull failed, parse the error message.
            errorMessage = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str()).message;
        }

        if (pullResult.value() == boost::beast::http::status::not_found)
        {
            THROW_HR_WITH_USER_ERROR(WSLA_E_IMAGE_NOT_FOUND, errorMessage);
        }
        else if (pullResult.value() == boost::beast::http::status::bad_request)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
        }
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::BuildImage(LPCWSTR ContextPath, ULONG DockerfileHandle, LPCSTR ImageTag, IProgressCallback* ProgressCallback)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ContextPath);
    RETURN_HR_IF(E_INVALIDARG, *ContextPath == L'\0');

    wil::unique_handle dockerfileFileHandle;
    if (DockerfileHandle != 0 && DockerfileHandle != HandleToULong(INVALID_HANDLE_VALUE))
    {
        dockerfileFileHandle.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(DockerfileHandle)));
    }

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    GUID volumeId{};
    THROW_IF_FAILED(CoCreateGuid(&volumeId));
    auto mountPath = std::format("/mnt/{}", wsl::shared::string::GuidToString<char>(volumeId));
    THROW_IF_FAILED(m_virtualMachine->MountWindowsFolder(ContextPath, mountPath.c_str(), TRUE));
    auto unmountFolder =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { m_virtualMachine->UnmountWindowsFolder(mountPath.c_str()); });

    std::vector<std::string> buildArgs{"/usr/bin/docker", "build", "--progress=rawjson"};
    if (ImageTag != nullptr && *ImageTag != '\0')
    {
        buildArgs.push_back("-t");
        buildArgs.push_back(ImageTag);
    }
    if (dockerfileFileHandle)
    {
        buildArgs.push_back("-f");
        buildArgs.push_back("-");
    }
    buildArgs.push_back(mountPath);

    WSL_LOG("BuildImageStart", TraceLoggingValue(wsl::shared::string::Join(buildArgs, ' ').c_str(), "Command"));

    ServiceProcessLauncher buildLauncher(buildArgs[0], buildArgs, {}, dockerfileFileHandle ? WSLAProcessFlagsStdin : WSLAProcessFlagsNone);
    auto buildProcess = buildLauncher.Launch(*m_virtualMachine);

    auto io = CreateIOContext();

    if (dockerfileFileHandle)
    {
        io.AddHandle(std::make_unique<relay::RelayHandle<relay::ReadHandle>>(
            common::relay::HandleWrapper{std::move(dockerfileFileHandle)},
            common::relay::HandleWrapper{buildProcess.GetStdHandle(WSLAFDStdin)}));
    }

    std::string allOutput;
    std::string pendingJson;
    std::set<std::string> reportedSteps;
    std::set<std::string> reportedErrors;

    auto reportProgress = [&](const std::string& message) {
        if (ProgressCallback != nullptr)
        {
            THROW_IF_FAILED(ProgressCallback->OnProgress(message.c_str(), "", 0, 0));
        }
    };

    // Accumulate lines and use accept() to detect complete JSON objects. Check for non-JSON lines between JSON objects and add
    // them to the output in case they contain helpful information about the build.
    auto captureOutput = [&](const gsl::span<char>& content) {
        std::string line{content.begin(), content.end()};

        pendingJson.append(line);

        if (!nlohmann::json::accept(pendingJson))
        {
            if (pendingJson.empty() || pendingJson[0] != '{')
            {
                allOutput.append(pendingJson).append("\n");
                pendingJson.clear();
            }

            return;
        }

        auto json = nlohmann::json::parse(pendingJson);
        pendingJson.clear();

        docker_schema::BuildKitSolveStatus status{};
        from_json(json, status);

        for (const auto& vertex : status.vertexes)
        {
            bool isInternal = vertex.name.find("[internal]") != std::string::npos;

            if (!vertex.started.empty() && reportedSteps.insert(vertex.digest).second)
            {
                allOutput.append(vertex.name).append("\n");

                if (!isInternal && !vertex.name.empty() && vertex.name[0] == '[')
                {
                    reportProgress(vertex.name + "\n");
                }
            }

            if (!vertex.error.empty() && !isInternal && reportedErrors.insert(vertex.digest).second)
            {
                allOutput.append(vertex.error).append("\n");
                reportProgress(vertex.error + "\n");
            }
        }
    };

    // With --progress=rawjson, docker writes progress to stderr and the final image ID to stdout on success (empty on
    // failure). Stdout is drained into allOutput (shown only on error) and its EOF signals build completion.
    io.AddHandle(
        std::make_unique<relay::ReadHandle>(
            buildProcess.GetStdHandle(1), [&](const auto& content) { allOutput.append(content.begin(), content.end()); }),
        relay::MultiHandleWait::CancelOnCompleted);

    io.AddHandle(std::make_unique<relay::LineBasedReadHandle>(buildProcess.GetStdHandle(2), captureOutput, false));

    io.Run({});

    int exitCode = buildProcess.Wait();
    WSL_LOG("BuildImageComplete", TraceLoggingValue(exitCode, "ExitCode"));
    THROW_HR_WITH_USER_ERROR_IF(E_FAIL, allOutput, exitCode != 0);

    std::string tag = (ImageTag != nullptr && *ImageTag != '\0') ? ImageTag : "";
    reportProgress(tag.empty() ? "\nBuild complete.\n" : "\nBuild complete: " + tag + "\n");

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::LoadImage(ULONG ImageHandle, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

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

    COMServiceExecutionContext context;

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

    auto io = CreateIOContext();

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

    io.AddHandle(std::make_unique<relay::RelayHandle<relay::ReadHandle>>(
        common::relay::HandleWrapper{std::move(imageFileHandle)}, common::relay::HandleWrapper{Request.stream.native_handle()}));

    io.AddHandle(
        std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(Request, std::move(onHttpResponse), std::move(onProgress)),
        MultiHandleWait::CancelOnCompleted);

    io.Run({});

    THROW_HR_IF(E_UNEXPECTED, !importResult.has_value());

    if (importResult.value() != boost::beast::http::status::ok)
    {
        // Import failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());

        // TODO: Return error message to client.
        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message);
    }
}

HRESULT WSLASession::ExportContainer(ULONG OutHandle, LPCSTR ContainerID, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ContainerID);
    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->ExportContainer(ContainerID);
    ExportContainerImpl(retVal, OutHandle);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::ExportContainerImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, ULONG OutputHandle)
{
    wil::unique_handle containerFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto io = CreateIOContext();

    std::string errorJson;

    if (SocketCodePair.first != 200)
    {
        auto accumulateError = [&](const gsl::span<char>& buffer) {
            // If the export failed, accumulate the error message.
            errorJson.append(buffer.data(), buffer.size());
        };

        io.AddHandle(
            std::make_unique<relay::ReadHandle>(common::relay::HandleWrapper{std::move(SocketCodePair.second)}, std::move(accumulateError)),
            MultiHandleWait::CancelOnCompleted);
    }
    else
    {
        io.AddHandle(
            std::make_unique<relay::RelayHandle<relay::HTTPChunkBasedReadHandle>>(
                common::relay::HandleWrapper{std::move(SocketCodePair.second)}, common::relay::HandleWrapper{std::move(containerFileHandle)}),
            MultiHandleWait::CancelOnCompleted);
    }

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Export failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());

        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_CONTAINER_NOT_FOUND, error.message, SocketCodePair.first == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message);
    }
}

HRESULT WSLASession::SaveImage(ULONG OutHandle, LPCSTR ImageNameOrID, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrID);
    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->SaveImage(ImageNameOrID);
    SaveImageImpl(retVal, OutHandle);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, ULONG OutputHandle)
{
    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto io = CreateIOContext();

    std::string errorJson;

    if (SocketCodePair.first != 200)
    {
        auto accumulateError = [&](const gsl::span<char>& buffer) {
            // If the save failed, accumulate the error message.
            errorJson.append(buffer.data(), buffer.size());
        };

        io.AddHandle(
            std::make_unique<relay::ReadHandle>(common::relay::HandleWrapper{std::move(SocketCodePair.second)}, std::move(accumulateError)),
            MultiHandleWait::CancelOnCompleted);
    }
    else
    {
        io.AddHandle(
            std::make_unique<relay::RelayHandle<relay::HTTPChunkBasedReadHandle>>(
                common::relay::HandleWrapper{std::move(SocketCodePair.second)}, common::relay::HandleWrapper{std::move(imageFileHandle)}),
            MultiHandleWait::CancelOnCompleted);
    }

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Save failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());
        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message.c_str());
    }
}

HRESULT WSLASession::ListImages(WSLA_IMAGE_INFORMATION** Images, ULONG* Count)
try
{
    COMServiceExecutionContext context;

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

HRESULT WSLASession::DeleteImage(const WSLA_DELETE_IMAGE_OPTIONS* Options, WSLA_DELETED_IMAGE_INFORMATION** DeletedImages, ULONG* Count)
try
{
    COMServiceExecutionContext context;

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
        }

        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
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

HRESULT WSLASession::TagImage(const WSLA_TAG_IMAGE_OPTIONS* Options)
try
{
    COMServiceExecutionContext context;

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
        }

        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), errorMessage, e.StatusCode() == 400);
        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateContainer(const WSLA_CONTAINER_OPTIONS* containerOptions, IWSLAContainer** Container)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, containerOptions);

    // Validate that Image is not null.
    RETURN_HR_IF(E_INVALIDARG, containerOptions->Image == nullptr);

    std::lock_guard lock{m_lock};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Validate that name & images are valid.
    if (containerOptions->Name != nullptr)
    {
        ValidateContainerName(containerOptions->Name);
    }

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

        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }
}
CATCH_RETURN();

HRESULT WSLASession::OpenContainer(LPCSTR Id, IWSLAContainer** Container)
try
{
    COMServiceExecutionContext context;

    ValidateContainerName(Id);

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
    COMServiceExecutionContext context;

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
    COMServiceExecutionContext context;

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
    COMServiceExecutionContext context;

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessagePathNotAbsolute(Path), !std::filesystem::path(Path).is_absolute());

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

    m_terminated = true;
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::MountWindowsFolder(LPCWSTR WindowsPath, LPCSTR LinuxPath, BOOL ReadOnly)
try
{
    COMServiceExecutionContext context;

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}
CATCH_RETURN();

HRESULT WSLASession::UnmountWindowsFolder(LPCSTR LinuxPath)
try
{
    COMServiceExecutionContext context;

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->UnmountWindowsFolder(LinuxPath);
}
CATCH_RETURN();

HRESULT WSLASession::MapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    COMServiceExecutionContext context;

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->MapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::UnmapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    COMServiceExecutionContext context;

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->UnmapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLASession) ? S_OK : S_FALSE;
}

// TODO consider allowing callers to pass cancellation handles.
MultiHandleWait WSLASession::CreateIOContext()
{
    relay::MultiHandleWait io;

    // Cancel with E_ABORT if the session is terminating.
    io.AddHandle(std::make_unique<relay::EventHandle>(
        m_sessionTerminatingEvent.get(), [this]() { THROW_HR_MSG(E_ABORT, "Session %lu is terminating", m_id); }));

    // Cancel with E_ABORT if the client process exits.
    io.AddHandle(std::make_unique<relay::EventHandle>(
        wslutil::OpenCallingProcess(SYNCHRONIZE), [this]() { THROW_HR_MSG(E_ABORT, "Client process has exited"); }));

    return io;
}

void WSLASession::OnContainerDeleted(const WSLAContainerImpl* Container)
{
    std::lock_guard lock{m_lock};
    WI_VERIFY(std::erase_if(m_containers, [Container](const auto& e) { return e.get() == Container; }) == 1);
}

HRESULT WSLASession::GetState(_Out_ WSLASessionState* State)
{
    *State = m_terminated ? WSLASessionStateTerminated : WSLASessionStateRunning;
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
