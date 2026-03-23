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

void ValidateName(LPCSTR Name)
{
    const auto& locale = std::locale::classic();
    size_t i = 0;

    for (; Name[i] != '\0'; i++)
    {
        if (!std::isalnum(Name[i], locale) && Name[i] != '_' && Name[i] != '-' && Name[i] != '.')
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslaInvalidName(Name));
        }
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslaInvalidName(Name), i == 0 || i > WSLA_MAX_CONTAINER_NAME_LENGTH);
}

wsla_schema::InspectImage ConvertInspectImage(const docker_schema::InspectImage& dockerInspect)
{
    wsla_schema::InspectImage wslaInspect{};

    // Direct field mappings
    wslaInspect.Id = dockerInspect.Id;
    wslaInspect.RepoTags = dockerInspect.RepoTags;
    wslaInspect.RepoDigests = dockerInspect.RepoDigests;
    wslaInspect.Parent = dockerInspect.Parent;
    wslaInspect.Comment = dockerInspect.Comment;
    wslaInspect.Created = dockerInspect.Created;
    wslaInspect.Author = dockerInspect.Author;
    wslaInspect.Architecture = dockerInspect.Architecture;
    wslaInspect.Os = dockerInspect.Os;
    wslaInspect.Size = dockerInspect.Size;
    wslaInspect.Metadata = dockerInspect.Metadata;

    // Convert Config from docker_schema to wsla_schema
    if (dockerInspect.Config.has_value())
    {
        wsla_schema::ImageConfig wslaConfig{};
        const auto& dockerConfig = dockerInspect.Config.value();

        wslaConfig.Cmd = dockerConfig.Cmd;
        wslaConfig.Entrypoint = dockerConfig.Entrypoint;
        wslaConfig.Env = dockerConfig.Env;
        wslaConfig.Labels = dockerConfig.Labels;
        wslaConfig.User = dockerConfig.User;
        wslaConfig.WorkingDir = dockerConfig.WorkingDir;

        wslaInspect.Config = wslaConfig;
    }

    return wslaInspect;
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

HRESULT WSLASession::Initialize(_In_ const WSLASessionInitSettings* Settings, _In_ IWSLAVirtualMachine* Vm)
try
{
    RETURN_HR_IF(E_POINTER, Settings == nullptr || Vm == nullptr);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_virtualMachine.has_value());

    // N.B. No locking is required because Initialize() is always called before the session is returned to the caller.
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

    m_virtualMachine->Initialize();

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
    RecoverExistingVolumes();
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

void WSLASession::ConfigureStorage(const WSLASessionInitSettings& Settings, PSID UserSid)
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
        m_virtualMachine->Ext4Format(diskDevice);
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

HRESULT WSLASession::PullImage(LPCSTR ImageUri, const WslaRegistryAuthInformation* RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(RegistryAuthenticationInformation);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageUri);

    auto [repo, tag] = wslutil::ParseImage(ImageUri);

    if (!tag.has_value())
    {
        tag = "latest";
    }

    auto lock = m_lock.lock_shared();

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

HRESULT WSLASession::BuildImage(const WSLABuildImageOptions* Options, IProgressCallback* ProgressCallback)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->ContextPath);
    RETURN_HR_IF(E_INVALIDARG, *Options->ContextPath == L'\0');
    RETURN_HR_IF(E_INVALIDARG, Options->Tags.Count > 0 && Options->Tags.Values == nullptr);
    RETURN_HR_IF(E_INVALIDARG, Options->BuildArgs.Count > 0 && Options->BuildArgs.Values == nullptr);

    wil::unique_handle dockerfileFileHandle;
    if (Options->DockerfileHandle != 0 && Options->DockerfileHandle != HandleToULong(INVALID_HANDLE_VALUE))
    {
        dockerfileFileHandle.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(Options->DockerfileHandle)));
    }

    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    GUID volumeId{};
    THROW_IF_FAILED(CoCreateGuid(&volumeId));
    auto mountPath = std::format("/mnt/{}", wsl::shared::string::GuidToString<char>(volumeId));
    THROW_IF_FAILED(m_virtualMachine->MountWindowsFolder(Options->ContextPath, mountPath.c_str(), TRUE));
    auto unmountFolder =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { m_virtualMachine->UnmountWindowsFolder(mountPath.c_str()); });

    std::vector<std::string> buildArgs{"/usr/bin/docker", "build", "--progress=rawjson"};
    for (ULONG i = 0; i < Options->Tags.Count; i++)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, Options->Tags.Values[i]);
        RETURN_HR_IF(E_INVALIDARG, strlen(Options->Tags.Values[i]) > WSLA_MAX_IMAGE_NAME_LENGTH);
        buildArgs.push_back("-t");
        buildArgs.push_back(Options->Tags.Values[i]);
    }
    for (ULONG i = 0; i < Options->BuildArgs.Count; i++)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, Options->BuildArgs.Values[i]);
        RETURN_HR_IF(E_INVALIDARG, Options->BuildArgs.Values[i][0] == '-');
        buildArgs.push_back("--build-arg");
        buildArgs.push_back(Options->BuildArgs.Values[i]);
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

    bool verbose = Options->Verbose;
    std::string allOutput;
    std::string pendingJson;
    std::set<std::string> reportedSteps;
    std::set<std::string> reportedErrors;
    std::string exportingVertexDigest;

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

                if (verbose || (!isInternal && !vertex.name.empty() && vertex.name[0] == '['))
                {
                    reportProgress(vertex.name + "\n");
                }

                // Track the "exporting to image" vertex so we can report its statuses (image hash, tags)
                // in non-verbose mode. This is a well-known BuildKit vertex name.
                if (vertex.name == "exporting to image")
                {
                    exportingVertexDigest = vertex.digest;
                }
            }

            if (!vertex.error.empty() && !isInternal && reportedErrors.insert(vertex.digest).second)
            {
                allOutput.append(vertex.error).append("\n");
                reportProgress(vertex.error + "\n");
            }
        }

        for (const auto& entry : status.statuses)
        {
            if (!entry.id.empty() && reportedSteps.insert(entry.id).second && (verbose || entry.vertex == exportingVertexDigest))
            {
                reportProgress(entry.id + "\n");
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

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::LoadImage(ULONG ImageHandle, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();

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
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageName) > WSLA_MAX_IMAGE_NAME_LENGTH);

    auto [repo, tag] = wslutil::ParseImage(ImageName);

    THROW_HR_IF_MSG(E_INVALIDARG, !tag.has_value(), "Expected tag for image import: %hs", ImageName);

    auto lock = m_lock.lock_shared();

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

    std::optional<std::string> pendingErrorJson;
    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG("ImageImportHttpResponse", TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        if (response.result_int() != 200)
        {
            auto it = response.find(boost::beast::http::field::content_type);

            THROW_HR_IF_MSG(
                E_UNEXPECTED,
                it == response.end() || !it->value().starts_with("application/json"),
                "Received HTTP %i but Content-Type is not json",
                response.result_int());

            pendingErrorJson.emplace();
        }
    };

    std::optional<std::string> errorMessage;
    auto onProgress = [&](const gsl::span<char>& buffer) {
        if (pendingErrorJson.has_value())
        {
            // If we received a non-200 status code, then the response body is an error message. Accumulate to the error message.
            pendingErrorJson->append(buffer.data(), buffer.size());
            return;
        }

        auto parsed = shared::FromJson<docker_schema::ImageLoadResult>(std::string(buffer.begin(), buffer.end()).c_str());

        if (parsed.errorDetail.has_value())
        {
            if (errorMessage.has_value())
            {
                LOG_HR_MSG(
                    E_UNEXPECTED,
                    "Overriding previous error message '%hs' with new message '%hs'",
                    errorMessage->c_str(),
                    parsed.errorDetail->message.c_str());
            }

            errorMessage = std::move(parsed.errorDetail->message);
        }
        else if (parsed.stream.has_value())
        {
            // TODO: report progress to caller.
            WSL_LOG("ImageImportProgress", TraceLoggingValue(parsed.stream->c_str(), "Content"));
        }
        else
        {
            LOG_HR_MSG(E_UNEXPECTED, "Failed to parse import progress: %.*hs", static_cast<int>(buffer.size()), buffer.data());
        }
    };

    io.AddHandle(std::make_unique<relay::RelayHandle<relay::ReadHandle>>(
        common::relay::HandleWrapper{std::move(imageFileHandle)}, common::relay::HandleWrapper{Request.stream.native_handle()}));

    io.AddHandle(
        std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(Request, std::move(onHttpResponse), std::move(onProgress)),
        MultiHandleWait::CancelOnCompleted);

    io.Run({});

    // Look for an error message returned as an HTTP response (non HTTP 200)
    if (pendingErrorJson.has_value())
    {
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(pendingErrorJson->c_str());

        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message);
    }

    // Otherwise look for an error message returned via the progress stream (HTTP 200 followed by a stream error).
    THROW_HR_WITH_USER_ERROR_IF(E_FAIL, errorMessage.value(), errorMessage.has_value());
}

HRESULT WSLASession::SaveImage(ULONG OutHandle, LPCSTR ImageNameOrID, IProgressCallback* ProgressCallback, HANDLE CancelEvent)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrID);
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageNameOrID) > WSLA_MAX_IMAGE_NAME_LENGTH);
    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->SaveImage(ImageNameOrID);
    SaveImageImpl(retVal, OutHandle, CancelEvent);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, ULONG OutputHandle, HANDLE CancelEvent)
{
    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto io = CreateIOContext(CancelEvent);

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

DEFINE_ENUM_FLAG_OPERATORS(WSLAListImagesFlags);

HRESULT WSLASession::ListImages(const WSLAListImageOptions* Options, WSLAImageInformation** Images, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Images);
    RETURN_HR_IF_NULL(E_POINTER, Count);

    *Count = 0;
    *Images = nullptr;

    if (Options != nullptr)
    {
        RETURN_HR_IF(E_INVALIDARG, WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsDanglingTrue) && WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsDanglingFalse));
        RETURN_HR_IF(E_INVALIDARG, Options->Reference != nullptr && strlen(Options->Reference) > WSLA_MAX_IMAGE_NAME_LENGTH);
    }

    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    // Extract options for Docker API
    bool all = false;
    bool digests = false;
    DockerHTTPClient::ListImagesFilters filters;

    if (Options != nullptr)
    {
        all = WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsAll);
        digests = WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsDigests);

        if (Options->Reference != nullptr)
        {
            filters.reference = Options->Reference;
        }

        if (Options->Before != nullptr)
        {
            filters.before = Options->Before;
        }

        if (Options->Since != nullptr)
        {
            filters.since = Options->Since;
        }

        // Check dangling flags (mutually exclusive in practice)
        if (WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsDanglingTrue))
        {
            filters.dangling = true;
        }
        else if (WI_IsFlagSet(Options->Flags, WSLAListImagesFlagsDanglingFalse))
        {
            filters.dangling = false;
        }
        // If neither flag is set, filters.dangling remains std::nullopt (show all)

        // Construct labels
        if (Options->Labels != nullptr && Options->LabelsCount > 0)
        {
            for (ULONG i = 0; i < Options->LabelsCount; ++i)
            {
                const auto& label = Options->Labels[i];
                if (label.Key != nullptr)
                {
                    std::string labelFilter = label.Key;
                    if (label.Value != nullptr)
                    {
                        labelFilter += "=";
                        labelFilter += label.Value;
                    }
                    filters.labels.push_back(labelFilter);
                }
            }
        }
    }

    std::vector<docker_schema::Image> images;
    try
    {
        images = m_dockerClient->ListImages(all, digests, filters);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to list images");

    // Compute the number of entries - one entry per tag, or one per image if no tags
    auto entries = std::accumulate<decltype(images.begin()), size_t>(images.begin(), images.end(), 0, [](auto sum, const auto& e) {
        return sum + (e.RepoTags.empty() ? 1 : e.RepoTags.size());
    });

    auto output = wil::make_unique_cotaskmem<WSLAImageInformation[]>(entries);

    size_t index = 0;
    for (const auto& e : images)
    {
        // Build a map from repo name to digest for this image
        // RepoDigests format: "repo@sha256:digest"
        std::map<std::string, std::string> repoToDigest;
        for (const auto& repoDigest : e.RepoDigests)
        {
            size_t atPos = repoDigest.find('@');
            THROW_HR_IF(E_UNEXPECTED, atPos == std::string::npos || atPos == 0);
            std::string repoName = repoDigest.substr(0, atPos);
            repoToDigest[repoName] = repoDigest;
        }

        if (e.RepoTags.empty())
        {
            // Image has no tags (dangling image)
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, "<none>:<none>") != 0);
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Hash, e.Id.c_str()) != 0);

            // Set digest if available
            if (!e.RepoDigests.empty())
            {
                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Digest, e.RepoDigests[0].c_str()) != 0);
            }
            else
            {
                output[index].Digest[0] = '\0';
            }

            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].ParentId, e.ParentId.c_str()) != 0);
            output[index].Size = e.Size;
            output[index].Created = e.Created;
            index++;
        }
        else
        {
            // Image has tags - create one entry per tag
            for (const auto& tag : e.RepoTags)
            {
                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, tag.c_str()) != 0);
                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Hash, e.Id.c_str()) != 0);

                // Extract repo name from tag (format: "repo:tag")
                // and lookup corresponding digest from the map
                auto repoName = wslutil::ParseImage(tag).first;
                size_t colonPos = tag.find(':');
                auto it = repoToDigest.find(repoName);
                if (it != repoToDigest.end())
                {
                    THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Digest, it->second.c_str()) != 0);
                }
                else
                {
                    output[index].Digest[0] = '\0';
                }

                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].ParentId, e.ParentId.c_str()) != 0);
                output[index].Size = e.Size;
                output[index].Created = e.Created;
                index++;
            }
        }
    }

    WI_ASSERT(index == entries);

    *Count = static_cast<ULONG>(entries);
    *Images = output.release();
    return S_OK;
}
CATCH_RETURN();

DEFINE_ENUM_FLAG_OPERATORS(WSLADeleteImageFlags);

HRESULT WSLASession::DeleteImage(const WSLADeleteImageOptions* Options, WSLADeletedImageInformation** DeletedImages, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);
    RETURN_HR_IF_NULL(E_POINTER, DeletedImages);
    RETURN_HR_IF_NULL(E_POINTER, Count);

    *DeletedImages = nullptr;
    *Count = 0;

    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    std::vector<docker_schema::DeletedImage> deletedImages;
    try
    {
        deletedImages = m_dockerClient->DeleteImage(
            Options->Image, WI_IsFlagSet(Options->Flags, WSLADeleteImageFlagsForce), WI_IsFlagSet(Options->Flags, WSLADeleteImageFlagsNoPrune));
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

    auto output = wil::make_unique_cotaskmem<WSLADeletedImageInformation[]>(deletedImages.size());

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

HRESULT WSLASession::TagImage(const WSLATagImageOptions* Options)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);
    RETURN_HR_IF_NULL(E_POINTER, Options->Repo);
    RETURN_HR_IF_NULL(E_POINTER, Options->Tag);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Repo) + strlen(Options->Tag) + 1 > WSLA_MAX_IMAGE_NAME_LENGTH);

    auto lock = m_lock.lock_shared();

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

HRESULT WSLASession::InspectImage(_In_ LPCSTR ImageNameOrId, _Out_ LPSTR* Output)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrId);
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageNameOrId) > WSLA_MAX_IMAGE_NAME_LENGTH);
    RETURN_HR_IF_NULL(E_POINTER, Output);

    *Output = nullptr;

    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    docker_schema::InspectImage dockerInspect;
    try
    {
        dockerInspect = m_dockerClient->InspectImage(ImageNameOrId);
    }
    catch (const DockerHTTPException& e)
    {
        std::string errorMessage = "Failed to inspect image";
        if (e.HasErrorMessage())
        {
            errorMessage = e.DockerMessage<docker_schema::ErrorResponse>().message;
        }

        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), errorMessage, e.StatusCode() == 400);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }

    // Convert to WSLA schema
    auto wslaInspect = ConvertInspectImage(dockerInspect);

    // Serialize to JSON
    std::string wslaJson = wsl::shared::ToJson(wslaInspect);
    *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wslaJson.c_str()).release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateContainer(const WSLAContainerOptions* containerOptions, IWSLAContainer** Container)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, containerOptions);

    // Validate that Image is not null.
    RETURN_HR_IF(E_INVALIDARG, containerOptions->Image == nullptr);

    auto lock = m_lock.lock_shared();

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_eventTracker);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient);

    // Validate that name & images are valid.
    if (containerOptions->Name != nullptr)
    {
        ValidateName(containerOptions->Name);
    }

    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);

    // TODO: Log entrance into the function.

    try
    {
        std::scoped_lock lock(m_containersLock, m_volumesLock);

        auto& it = m_containers.emplace_back(WSLAContainerImpl::Create(
            *containerOptions,
            *this,
            m_virtualMachine.value(),
            m_volumes,
            std::bind(&WSLASession::OnContainerDeleted, this, std::placeholders::_1),
            m_eventTracker.value(),
            m_dockerClient.value(),
            m_ioRelay));

        it->CopyTo(Container);

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

    ValidateName(Id);

    // Look for an exact ID match first.
    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock{m_containersLock};

    // Purge containers that were auto-deleted via OnEvent (--rm).
    std::erase_if(m_containers, [](const auto& e) { return e->State() == WslaContainerStateDeleted; });
    auto it = std::ranges::find_if(m_containers, [Id](const auto& e) { return e->ID() == Id; });

    // If no match is found, call Inspect() so that partial IDs and names are matched.
    if (it == m_containers.end())
    {
        // TODO: consider a trimmed down version of inspect to avoid parsing the full response.
        docker_schema::InspectContainer inspectResult;

        try
        {
            inspectResult = m_dockerClient->InspectContainer(Id);
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

    auto result = wil::ResultFromException([&]() { (*it)->CopyTo(Container); });

    // Return ERROR_NOT_FOUND if the container was found, but is being deleted for consistency.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), result == RPC_E_DISCONNECTED);

    return result;
}
CATCH_RETURN();

HRESULT WSLASession::ListContainers(WSLAContainerEntry** Containers, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    *Count = 0;
    *Containers = nullptr;

    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock{m_containersLock};

    // Purge containers that were auto-deleted via OnEvent (--rm).
    std::erase_if(m_containers, [](const auto& e) { return e->State() == WslaContainerStateDeleted; });

    auto output = wil::make_unique_cotaskmem<WSLAContainerEntry[]>(m_containers.size());

    size_t index = 0;
    for (const auto& e : m_containers)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, e->Image().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Name, e->Name().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Id, e->ID().c_str()) != 0);
        e->GetState(&output[index].State);
        e->GetStateChangedAt(&output[index].StateChangedAt);
        e->GetCreatedAt(&output[index].CreatedAt);
        index++;
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::PruneContainers(_In_opt_ WSLAContainerPruneFilter* Filters, _In_ DWORD FiltersCount, _In_ ULONGLONG Until, _Out_ WSLAPruneContainersResults* Result)
try
{
    COMServiceExecutionContext context;

    std::optional<docker_schema::PruneContainerLabelFilter> filters;

    if (Until > 0 || FiltersCount > 0)
    {
        THROW_HR_IF(E_POINTER, FiltersCount > 0 && Filters == nullptr);

        filters.emplace();

        for (DWORD i = 0; i < FiltersCount; ++i)
        {
            THROW_HR_IF_MSG(E_POINTER, Filters[i].Key == nullptr, "Filter key cannot be null (index %lu)", i);
            std::string labelFilter = Filters[i].Key;

            if (Filters[i].Value != nullptr)
            {
                labelFilter += '=';
                labelFilter += Filters[i].Value;
            }

            if (Filters[i].Present)
            {
                filters->presentLabels.emplace(std::move(labelFilter), true);
            }
            else
            {
                filters->absentLabels.emplace(std::move(labelFilter), true);
            }
        }

        if (Until > 0)
        {
            filters->until = Until;
        }
    }

    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    std::lock_guard containersLock{m_containersLock};

    docker_schema::PruneContainerResult pruneResult;

    try
    {
        pruneResult = m_dockerClient->PruneContainers(filters);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to prune containers");

    Result->SpaceReclaimed = pruneResult.SpaceReclaimed;

    if (pruneResult.ContainersDeleted.has_value() && pruneResult.ContainersDeleted->size() > 0)
    {
        // Remove deleted containers from m_containers.
        auto pred = [&](const auto& e) {
            return std::ranges::find(pruneResult.ContainersDeleted.value(), e->ID()) != pruneResult.ContainersDeleted->end();
        };

        auto erased = std::erase_if(m_containers, pred);
        LOG_HR_IF_MSG(
            E_UNEXPECTED,
            erased != pruneResult.ContainersDeleted->size(),
            "Expected to erase %zu containers, but erased %zu",
            pruneResult.ContainersDeleted->size(),
            erased);

        auto containers = wil::make_unique_cotaskmem<WSLAContainerId[]>(pruneResult.ContainersDeleted->size());

        for (size_t i = 0; i < pruneResult.ContainersDeleted->size(); ++i)
        {
            THROW_HR_IF_MSG(
                E_UNEXPECTED,
                strcpy_s(containers[i], pruneResult.ContainersDeleted.value()[i].c_str()) != 0,
                "Unexpected container name: %hs",
                pruneResult.ContainersDeleted.value()[i].c_str());
        }

        Result->Containers = containers.release();
        Result->ContainersCount = static_cast<DWORD>(pruneResult.ContainersDeleted->size());
    }
    else
    {
        Result->Containers = nullptr;
        Result->ContainersCount = 0;
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateRootNamespaceProcess(LPCSTR Executable, const WSLAProcessOptions* Options, IWSLAProcess** Process, int* Errno)
try
{
    COMServiceExecutionContext context;

    if (Errno != nullptr)
    {
        *Errno = -1; // Make sure not to return 0 if something fails.
    }

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    auto process = m_virtualMachine->CreateLinuxProcess(Executable, *Options, Errno);
    THROW_IF_FAILED(process.CopyTo(Process));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::FormatVirtualDisk(LPCWSTR Path)
try
{
    COMServiceExecutionContext context;

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessagePathNotAbsolute(Path), !std::filesystem::path(Path).is_absolute());

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Attach the disk to the VM (AttachDisk() performs the access check for the VHD file).
    auto [lun, device] = m_virtualMachine->AttachDisk(Path, false);

    // N.B. DetachDisk calls sync() before detaching.
    auto detachDisk = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this, lun]() { m_virtualMachine->DetachDisk(lun); });

    // Format it to ext4.
    m_virtualMachine->Ext4Format(device);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateVolume(const WSLAVolumeOptions* Options)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Name);
    RETURN_HR_IF_NULL(E_POINTER, Options->Type);

    std::string name = Options->Name;
    std::string type = Options->Type;

    ValidateName(name.c_str());

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    std::lock_guard volumesLock(m_volumesLock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_volumes.contains(name));
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslaInvalidVolumeType(type), type != "vhd");

    auto volume = WSLAVhdVolumeImpl::Create(*Options, m_storageVhdPath.parent_path(), m_virtualMachine.value(), m_dockerClient.value());
    auto [it, inserted] = m_volumes.insert({name, std::move(volume)});
    WI_VERIFY(inserted);

    WSL_LOG("VolumeCreated", TraceLoggingValue(name.c_str(), "VolumeName"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::DeleteVolume(LPCSTR Name)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Name);
    std::string name = Name;
    ValidateName(name.c_str());

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    std::lock_guard volumesLock(m_volumesLock);

    auto it = m_volumes.find(name);
    THROW_HR_WITH_USER_ERROR_IF(WSLA_E_VOLUME_NOT_FOUND, Localization::MessageWslaVolumeNotFound(name), it == m_volumes.end());

    it->second->Delete();
    m_volumes.erase(it);
    WSL_LOG("VolumeDeleted", TraceLoggingValue(name.c_str(), "VolumeName"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::Terminate()
try
{
    // m_sessionTerminatingEvent is always valid, so it can be signalled with the lock.
    // This allows a session to be unblocked if a stuck operation is holding the lock.
    m_sessionTerminatingEvent.SetEvent();

    // Acquire an exclusive lock to ensure that no operation is running.
    auto lock = m_lock.lock_exclusive();
    std::lock_guard containersLock(m_containersLock);
    std::lock_guard volumesLock(m_volumesLock);

    m_containers.clear();
    m_volumes.clear();

    // Stop the IO relay.
    // This stops:
    // - container state monitoring.
    // - container init process relays
    // - execs relays
    // - container logs relays
    m_ioRelay.Stop();

    {
        std::lock_guard allocatedPortsLock(m_allocatedPortsLock);
        m_allocatedPorts.clear();
    }

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
            try
            {
                m_dockerdProcess->Get().Signal(WSLASignalSIGKILL);
                exitCode = m_dockerdProcess->Wait(10 * 1000);
            }
            CATCH_LOG();
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

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}
CATCH_RETURN();

HRESULT WSLASession::UnmountWindowsFolder(LPCSTR LinuxPath)
try
{
    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->UnmountWindowsFolder(LinuxPath);
}
CATCH_RETURN();

HRESULT WSLASession::MapVmPort(int Family, unsigned short WindowsPort, unsigned short LinuxPort)
try
{
    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    std::lock_guard allocatedPortsLock(m_allocatedPortsLock);

    // Look for an existing allocation first.
    auto it = m_allocatedPorts.find(LinuxPort);

    bool inserted = false;
    auto cleanup = wil::scope_exit([&]() {
        if (inserted)
        {
            m_allocatedPorts.erase(it);
        }
    });

    if (it == m_allocatedPorts.end())
    {
        // No existing port allocation, create a new one.
        auto allocated = std::make_pair(m_virtualMachine->TryAllocatePort(LinuxPort, Family, IPPROTO_TCP), static_cast<size_t>(0));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), allocated.first == nullptr);

        it = m_allocatedPorts.emplace(LinuxPort, allocated).first;
        inserted = true;
    }

    auto mapping = VMPortMapping::LocalhostTcpMapping(Family, WindowsPort);
    mapping.AssignVmPort(it->second.first);

    m_virtualMachine->MapPort(mapping);

    // Increase usage count.
    it->second.second++;

    mapping.Release();
    cleanup.release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::UnmapVmPort(int Family, unsigned short WindowsPort, unsigned short LinuxPort)
try
{
    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    std::lock_guard allocatedPortsLock(m_allocatedPortsLock);

    auto it = m_allocatedPorts.find(LinuxPort);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_allocatedPorts.end());

    auto mapping = VMPortMapping::LocalhostTcpMapping(Family, WindowsPort);
    mapping.AssignVmPort(it->second.first);
    mapping.Attach(m_virtualMachine.value());

    auto cleanup = wil::scope_exit([&]() { mapping.Release(); });

    m_virtualMachine->UnmapPort(mapping);

    it->second.second--;

    // If usage count drops to 0, release the port allocation.
    if (it->second.second == 0)
    {
        m_allocatedPorts.erase(it);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLASession) ? S_OK : S_FALSE;
}

MultiHandleWait WSLASession::CreateIOContext(HANDLE CancelHandle)
{
    relay::MultiHandleWait io;

    // Cancel with E_ABORT if the session is terminating.
    io.AddHandle(std::make_unique<relay::EventHandle>(
        m_sessionTerminatingEvent.get(), [this]() { THROW_HR_MSG(E_ABORT, "Session %lu is terminating", m_id); }));

    // Cancel with E_ABORT if the client process exits.
    io.AddHandle(std::make_unique<relay::EventHandle>(
        wslutil::OpenCallingProcess(SYNCHRONIZE), [this]() { THROW_HR_MSG(E_ABORT, "Client process has exited"); }));

    if (CancelHandle != nullptr)
    {
        io.AddHandle(
            std::make_unique<relay::EventHandle>(CancelHandle, []() { THROW_HR_MSG(E_ABORT, "Cancellation handle was signaled"); }));
    }

    return io;
}

void WSLASession::OnContainerDeleted(const WSLAContainerImpl* Container)
{
    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock(m_containersLock);

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
                *this,
                m_virtualMachine.value(),
                m_volumes,
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

void WSLASession::RecoverExistingVolumes()
{
    WI_ASSERT(m_dockerClient.has_value());
    WI_ASSERT(m_virtualMachine.has_value());

    auto volumes = m_dockerClient->ListVolumes();

    std::lock_guard volumesLock(m_volumesLock);

    for (const auto& volume : volumes)
    {
        if (!volume.Labels.contains(WSLAVolumeMetadataLabel))
        {
            continue;
        }

        try
        {
            WI_ASSERT(!m_volumes.contains(volume.Name));

            auto vhdVolume = WSLAVhdVolumeImpl::Open(volume, m_virtualMachine.value(), m_dockerClient.value());
            auto [_, inserted] = m_volumes.insert({volume.Name, std::move(vhdVolume)});
            WI_VERIFY(inserted);
        }
        CATCH_LOG_MSG("Failed to recover volume: %hs", volume.Name.c_str());
    }

    WSL_LOG(
        "VolumesRecovered",
        TraceLoggingValue(m_displayName.c_str(), "SessionName"),
        TraceLoggingValue(m_volumes.size(), "VolumeCount"));
}

} // namespace wsl::windows::service::wsla
