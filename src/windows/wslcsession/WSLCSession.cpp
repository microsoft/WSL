/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSession.cpp

Abstract:

    This file contains the implementation of the WSLCSession COM class.

--*/

#include "precomp.h"
#include "WSLCSession.h"
#include "WSLCContainer.h"
#include "ServiceProcessLauncher.h"
#include "WslCoreFilesystem.h"

using namespace wsl::windows::common;
using relay::MultiHandleWait;
using wsl::shared::Localization;
using wsl::windows::service::wslc::UserCOMCallback;
using wsl::windows::service::wslc::UserHandle;
using wsl::windows::service::wslc::WSLCSession;
using wsl::windows::service::wslc::WSLCVirtualMachine;

constexpr auto c_containerdStorage = "/var/lib/docker";

namespace {

std::string IndentLines(const std::string& input, const std::string& prefix)
{
    if (input.empty())
    {
        return {};
    }

    std::string result = prefix;
    for (size_t i = 0; i < input.size(); i++)
    {
        result.push_back(input[i]);
        if (i + 1 < input.size())
        {
            if (input[i] == '\n' || (input[i] == '\r' && input[i + 1] != '\n'))
            {
                result.append(prefix);
            }
        }
    }

    return result;
}

void ValidateName(LPCSTR Name)
{
    const auto& locale = std::locale::classic();
    size_t i = 0;

    for (; Name[i] != '\0'; i++)
    {
        if (!std::isalnum(Name[i], locale) && Name[i] != '_' && Name[i] != '-' && Name[i] != '.')
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcInvalidName(Name));
        }
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcInvalidName(Name), i == 0 || i > WSLC_MAX_CONTAINER_NAME_LENGTH);
}

wslc_schema::InspectImage ConvertInspectImage(const docker_schema::InspectImage& dockerInspect)
{
    wslc_schema::InspectImage wslcInspect{};

    // Direct field mappings
    wslcInspect.Id = dockerInspect.Id;
    wslcInspect.RepoTags = dockerInspect.RepoTags;
    wslcInspect.RepoDigests = dockerInspect.RepoDigests;
    wslcInspect.Parent = dockerInspect.Parent;
    wslcInspect.Comment = dockerInspect.Comment;
    wslcInspect.Created = dockerInspect.Created;
    wslcInspect.Author = dockerInspect.Author;
    wslcInspect.Architecture = dockerInspect.Architecture;
    wslcInspect.Os = dockerInspect.Os;
    wslcInspect.Size = dockerInspect.Size;
    wslcInspect.Metadata = dockerInspect.Metadata;

    // Convert Config from docker_schema to wslc_schema
    if (dockerInspect.Config.has_value())
    {
        wslc_schema::ImageConfig wslcConfig{};
        const auto& dockerConfig = dockerInspect.Config.value();

        wslcConfig.Cmd = dockerConfig.Cmd;
        wslcConfig.Entrypoint = dockerConfig.Entrypoint;
        wslcConfig.Env = dockerConfig.Env;
        wslcConfig.Labels = dockerConfig.Labels;
        wslcConfig.User = dockerConfig.User;
        wslcConfig.WorkingDir = dockerConfig.WorkingDir;

        wslcInspect.Config = wslcConfig;
    }

    return wslcInspect;
}

} // namespace

namespace wsl::windows::service::wslc {

UserHandle::UserHandle(WSLCSession& Session, HANDLE handle) : m_session(&Session), m_handle(handle)
{
    WI_ASSERT(!!m_handle);
}

UserHandle::UserHandle(UserHandle&& Other)
{
    *this = std::move(Other);
}

UserHandle& UserHandle::operator=(UserHandle&& Other)
{
    if (this != &Other)
    {
        Reset();
        m_session = Other.m_session;
        m_handle = Other.m_handle;

        Other.m_handle = nullptr;
        Other.m_session = nullptr;
    }
    return *this;
}

void UserHandle::Reset()
{
    if (m_handle != nullptr)
    {
        WI_ASSERT(m_session != nullptr);

        m_session->ReleaseUserHandle(m_handle);
        m_handle = nullptr;
    }
}

UserHandle::~UserHandle()
{
    Reset();
}

HANDLE UserHandle::Get() const noexcept
{
    return m_handle;
}

UserCOMCallback::UserCOMCallback(WSLCSession& Session) noexcept : m_session(&Session), m_threadId(GetCurrentThreadId())
{
}

UserCOMCallback::UserCOMCallback(UserCOMCallback&& Other) noexcept
{
    *this = std::move(Other);
}

UserCOMCallback& UserCOMCallback::operator=(UserCOMCallback&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        m_session = Other.m_session;
        m_threadId = Other.m_threadId;

        Other.m_threadId = 0;
        Other.m_session = nullptr;
    }
    return *this;
}

void UserCOMCallback::Reset() noexcept
{
    if (m_threadId != 0)
    {
        WI_ASSERT(m_session != nullptr);

        m_session->UnregisterUserCOMCallback(m_threadId);
        m_threadId = 0;

        LOG_IF_FAILED(CoDisableCallCancellation(nullptr));
    }
}

UserCOMCallback::~UserCOMCallback() noexcept
{
    Reset();
}

HRESULT WSLCSession::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    RETURN_HR_IF(E_POINTER, ProcessHandle == nullptr);

    wil::unique_handle process{OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, GetCurrentProcessId())};
    THROW_LAST_ERROR_IF(!process);

    *ProcessHandle = process.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::Initialize(_In_ const WSLCSessionInitSettings* Settings, _In_ IWSLCVirtualMachine* Vm)
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

    auto [_, __, channel] = m_virtualMachine->Fork(WSLC_FORK::Thread);

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

WSLCSession::~WSLCSession()
{
    WSL_LOG("SessionTerminated", TraceLoggingValue(m_id, "SessionId"), TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    LOG_IF_FAILED(Terminate());

    if (m_destructionCallback)
    {
        m_destructionCallback();
    }
}

void WSLCSession::SetDestructionCallback(std::function<void()>&& callback)
{
    m_destructionCallback = std::move(callback);
}

void WSLCSession::ConfigureStorage(const WSLCSessionInitSettings& Settings, PSID UserSid)
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

        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND),
            Localization::MessageWslcSessionStorageNotFound(Settings.StoragePath),
            WI_IsFlagSet(Settings.StorageFlags, WSLCSessionStorageFlagsNoCreate));

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

HRESULT WSLCSession::GetId(ULONG* Id)
{
    *Id = m_id;

    return S_OK;
}

void WSLCSession::OnDockerdExited()
{
    if (!m_sessionTerminatingEvent.is_signaled())
    {
        WSL_LOG("UnexpectedDockerdExit", TraceLoggingValue(m_displayName.c_str(), "Name"));
    }
}

void WSLCSession::OnDockerdLog(const gsl::span<char>& buffer)
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

void WSLCSession::StartDockerd()
{
    std::vector<std::string> args{{"/usr/bin/dockerd"}};

    if (WI_IsFlagSet(m_featureFlags, WslcFeatureFlagsDebug))
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
        m_dockerdProcess->GetExitEvent(), std::bind(&WSLCSession::OnDockerdExited, this)));
}

void WSLCSession::StreamImageOperation(DockerHTTPClient::HTTPRequestContext& requestContext, LPCSTR Image, LPCSTR OperationName, IProgressCallback* ProgressCallback)
{
    auto io = CreateIOContext();

    struct Response
    {
        boost::beast::http::status result;
        bool isJson = false;
    };

    std::optional<UserCOMCallback> comCall;
    if (ProgressCallback != nullptr)
    {
        comCall = RegisterUserCOMCallback();
    }

    std::optional<Response> httpResponse;

    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG(
            "ImageOperationHttpResponse",
            TraceLoggingValue(OperationName, "Operation"),
            TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        auto it = response.find(boost::beast::http::field::content_type);
        httpResponse.emplace(response.result(), it != response.end() && it->value().starts_with("application/json"));
    };

    std::string errorJson;
    std::optional<std::string> reportedError;
    auto onChunk = [&](const gsl::span<char>& Content) {
        if (httpResponse.has_value() && httpResponse->result != boost::beast::http::status::ok)
        {
            // If the status code is an error, then this is an error message, not a progress update.
            errorJson.append(Content.data(), Content.size());
            return;
        }

        std::string contentString{Content.begin(), Content.end()};
        WSL_LOG(
            "ImageOperationProgress",
            TraceLoggingValue(OperationName, "Operation"),
            TraceLoggingValue(Image, "Image"),
            TraceLoggingValue(contentString.c_str(), "Content"));

        auto parsed = wsl::shared::FromJson<docker_schema::CreateImageProgress>(contentString.c_str());

        if (parsed.errorDetail.has_value())
        {
            if (reportedError.has_value())
            {
                LOG_HR_MSG(
                    E_UNEXPECTED,
                    "Received multiple error messages during image %hs. Previous: %hs, New: %hs",
                    OperationName,
                    reportedError->c_str(),
                    parsed.errorDetail->message.c_str());
            }

            reportedError = parsed.errorDetail->message;
            return;
        }

        if (ProgressCallback != nullptr)
        {
            THROW_IF_FAILED(ProgressCallback->OnProgress(
                parsed.status.c_str(), parsed.id.c_str(), parsed.progressDetail.current, parsed.progressDetail.total));
        }
    };

    auto onCompleted = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(
        requestContext, std::move(onHttpResponse), std::move(onChunk), std::move(onCompleted)));

    io.Run({});

    THROW_HR_IF(E_UNEXPECTED, !httpResponse.has_value());

    if (httpResponse->result != boost::beast::http::status::ok)
    {
        std::string errorMessage;
        if (httpResponse->isJson)
        {
            // operation failed, parse the error message.
            errorMessage = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str()).message;
        }
        else
        {
            // If no error message was explicitly returned, use the response body, if any.
            errorMessage = errorJson;
        }

        if (httpResponse->result == boost::beast::http::status::not_found)
        {
            THROW_HR_WITH_USER_ERROR(WSLC_E_IMAGE_NOT_FOUND, errorMessage);
        }
        else if (httpResponse->result == boost::beast::http::status::bad_request)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
        }
    }
    else if (reportedError.has_value())
    {
        // Can happen if an error is returned during progress after receiving an OK status.
        THROW_HR_WITH_USER_ERROR(E_FAIL, reportedError.value().c_str());
    }
}

HRESULT WSLCSession::PullImage(LPCSTR Image, LPCSTR RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Image);

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto [repo, tagOrDigest] = wslutil::ParseImage(Image);

    if (!tagOrDigest.has_value())
    {
        tagOrDigest = "latest";
    }

    std::optional<std::string> registryAuth;

    if (RegistryAuthenticationInformation != nullptr && *RegistryAuthenticationInformation != '\0')
    {
        registryAuth = std::string(RegistryAuthenticationInformation);
    }

    auto requestContext = m_dockerClient->PullImage(repo, tagOrDigest, registryAuth);
    StreamImageOperation(*requestContext, Image, "Pull", ProgressCallback);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::BuildImage(const WSLCBuildImageOptions* Options, IProgressCallback* ProgressCallback, HANDLE CancelEvent)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->ContextPath);
    RETURN_HR_IF(E_INVALIDARG, *Options->ContextPath == L'\0');
    RETURN_HR_IF(E_INVALIDARG, Options->Tags.Count > 0 && Options->Tags.Values == nullptr);
    RETURN_HR_IF(E_INVALIDARG, Options->BuildArgs.Count > 0 && Options->BuildArgs.Values == nullptr);
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        WI_IsAnyFlagSet(static_cast<WSLCBuildImageFlags>(Options->Flags), ~WSLCBuildImageFlagsValid),
        "Invalid flags: 0x%x",
        Options->Flags);

    auto buildFileHandle = OpenUserHandle(Options->DockerfileHandle);

    std::optional<UserCOMCallback> comCall;
    if (ProgressCallback != nullptr)
    {
        comCall = RegisterUserCOMCallback();
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
    if (WI_IsFlagSet(Options->Flags, WSLCBuildImageFlagsNoCache))
    {
        buildArgs.push_back("--no-cache");
    }
    for (ULONG i = 0; i < Options->Tags.Count; i++)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, Options->Tags.Values[i]);
        RETURN_HR_IF(E_INVALIDARG, strlen(Options->Tags.Values[i]) > WSLC_MAX_IMAGE_NAME_LENGTH);
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

    buildArgs.push_back("-f");
    buildArgs.push_back("-");
    buildArgs.push_back(mountPath);

    WSL_LOG("BuildImageStart", TraceLoggingValue(wsl::shared::string::Join(buildArgs, ' ').c_str(), "Command"));

    ServiceProcessLauncher buildLauncher(buildArgs[0], buildArgs, {}, WSLCProcessFlagsStdin);
    auto buildProcess = buildLauncher.Launch(*m_virtualMachine);

    auto io = CreateIOContext();

    io.AddHandle(std::make_unique<relay::RelayHandle<relay::ReadHandle>>(
        buildFileHandle.Get(), common::relay::HandleWrapper{buildProcess.GetStdHandle(WSLCFDStdin)}));

    bool verbose = WI_IsFlagSet(Options->Flags, WSLCBuildImageFlagsVerbose);
    std::string allOutput;
    std::string pendingJson;
    std::set<std::string> reportedSteps;
    std::set<std::string> reportedErrors;
    std::map<std::string, std::string> digestToStageName;
    bool needsNewline = false; // true when the last log chunk didn't end with \n
    std::string lastLogVertex; // digest of the vertex that produced the last log output

    // Extract the named build stage from a BuildKit vertex name. Vertices within the same named stage
    // (e.g. "[builder 1/3]" and "[builder 2/3]") share a key. Returns empty for unnamed stages.
    auto getStageName = [](const std::string& name) -> std::string {
        if (name.size() < 2 || name[0] != '[')
        {
            return {};
        }

        auto close = name.find(']');
        if (close == std::string::npos)
        {
            return {};
        }

        // Pattern: "[name N/M]" or "[N/M]". The stage name is the part before "N/M".
        std::string content = name.substr(1, close - 1);
        auto slash = content.find('/');
        if (slash != std::string::npos)
        {
            auto space = content.rfind(' ', slash);
            if (space != std::string::npos)
            {
                return content.substr(0, space);
            }
        }

        return {};
    };

    auto logPrefix = [](const std::string& name) -> std::string {
        if (name.empty())
        {
            return "  | ";
        }
        return "  [" + name + "] ";
    };

    auto reportProgress = [&](const std::string& message) {
        if (ProgressCallback != nullptr)
        {
            THROW_IF_FAILED(ProgressCallback->OnProgress(message.c_str(), "", 0, 0));
        }
    };

    auto flushLine = [&]() {
        if (needsNewline)
        {
            reportProgress("\n");
            needsNewline = false;
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

        // Process vertices before logs so digestToStageName is populated for log correlation.
        for (const auto& vertex : status.vertexes)
        {
            if (!verbose && vertex.name.find("[internal]") != std::string::npos)
            {
                continue;
            }

            digestToStageName.try_emplace(vertex.digest, getStageName(vertex.name));

            if (!vertex.started.empty() && reportedSteps.insert(vertex.digest).second)
            {
                flushLine();
                reportProgress(vertex.name + "\n");
            }

            if (!vertex.error.empty() && reportedErrors.insert(vertex.digest).second)
            {
                flushLine();
                reportProgress(vertex.error + "\n");
            }
        }

        for (const auto& log : status.logs)
        {
            if (auto it = digestToStageName.find(log.vertex); it != digestToStageName.end() && !log.data.empty())
            {
                std::string decoded = wslutil::Base64Decode(log.data);
                if (!decoded.empty())
                {
                    if (log.vertex != lastLogVertex && decoded[0] != '\n')
                    {
                        flushLine();
                    }

                    // When continuing an unterminated line, emit the leading \n or \r directly
                    // so it terminates/overwrites cleanly without a spurious prefix.
                    if (needsNewline && (decoded[0] == '\n' || decoded[0] == '\r'))
                    {
                        reportProgress(decoded.substr(0, 1));
                        decoded.erase(0, 1);
                    }

                    if (!decoded.empty())
                    {
                        reportProgress(IndentLines(decoded, logPrefix(it->second)));
                    }

                    needsNewline = !decoded.empty() && decoded.back() != '\n';
                    lastLogVertex = log.vertex;
                }
            }
        }

        for (const auto& entry : status.statuses)
        {
            if (auto it = digestToStageName.find(entry.vertex);
                it != digestToStageName.end() && !entry.id.empty() && reportedSteps.insert(entry.id).second)
            {
                flushLine();
                reportProgress(logPrefix(it->second) + entry.id + "\n");
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

    // Handle cancellation within the IO loop (NeedNotComplete) so pipes keep draining.
    bool cancelled = false;
    wil::unique_handle killTimer;
    if (CancelEvent != nullptr)
    {
        killTimer.reset(CreateWaitableTimer(nullptr, TRUE, nullptr));
        THROW_LAST_ERROR_IF_NULL(killTimer);

        io.AddHandle(
            std::make_unique<relay::EventHandle>(
                CancelEvent,
                [&]() {
                    cancelled = true;
                    LOG_IF_FAILED(buildProcess.Get().Signal(WSLCSignalSIGTERM));
                    LARGE_INTEGER dueTime{.QuadPart = -10LL * 10 * 1000 * 1000}; // 10 seconds
                    THROW_IF_WIN32_BOOL_FALSE(SetWaitableTimer(killTimer.get(), &dueTime, 0, nullptr, nullptr, FALSE));
                }),
            relay::MultiHandleWait::NeedNotComplete);

        io.AddHandle(
            std::make_unique<relay::EventHandle>(
                killTimer.get(), [&]() { LOG_IF_FAILED(buildProcess.Get().Signal(WSLCSignalSIGKILL)); }),
            relay::MultiHandleWait::NeedNotComplete);
    }

    try
    {
        io.Run({});
    }
    catch (...)
    {
        flushLine();
        LOG_IF_FAILED(buildProcess.Get().Signal(WSLCSignalSIGTERM));
        try
        {
            buildProcess.Wait(10 * 1000);
        }
        catch (...)
        {
            if (wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
            {
                LOG_IF_FAILED(buildProcess.Get().Signal(WSLCSignalSIGKILL));
                try
                {
                    buildProcess.Wait(10 * 1000);
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION_MSG("Build process did not exit after SIGKILL");
                }
            }
        }
        throw;
    }

    flushLine();

    THROW_HR_IF_MSG(E_ABORT, cancelled, "Cancellation handle was signaled");

    int exitCode = buildProcess.Wait();
    WSL_LOG("BuildImageComplete", TraceLoggingValue(exitCode, "ExitCode"));
    THROW_HR_WITH_USER_ERROR_IF(E_FAIL, allOutput, exitCode != 0);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::LoadImage(const WSLCHandle ImageHandle, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
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

HRESULT WSLCSession::ImportImage(const WSLCHandle ImageHandle, LPCSTR ImageName, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageName);
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageName) > WSLC_MAX_IMAGE_NAME_LENGTH);

    auto [repo, tagOrDigest] = wslutil::ParseImage(ImageName);

    THROW_HR_IF_MSG(E_INVALIDARG, !tagOrDigest.has_value(), "Expected tag for image import: %hs", ImageName);

    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto requestContext = m_dockerClient->ImportImage(repo, tagOrDigest.value(), ContentSize);

    ImportImageImpl(*requestContext, ImageHandle);
    return S_OK;
}
CATCH_RETURN();

void WSLCSession::ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, const WSLCHandle ImageHandle)
{
    auto userHandle = OpenUserHandle(ImageHandle);

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
        userHandle.Get(), common::relay::HandleWrapper{Request.stream.native_handle()}));

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

HRESULT WSLCSession::SaveImage(WSLCHandle OutHandle, LPCSTR ImageNameOrID, IProgressCallback* ProgressCallback, HANDLE CancelEvent)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrID);
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageNameOrID) > WSLC_MAX_IMAGE_NAME_LENGTH);
    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto retVal = m_dockerClient->SaveImage(ImageNameOrID);
    SaveImageImpl(retVal, OutHandle, CancelEvent);
    return S_OK;
}
CATCH_RETURN();

void WSLCSession::SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& SocketCodePair, WSLCHandle OutputHandle, HANDLE CancelEvent)
{
    auto userHandle = OpenUserHandle(OutputHandle);

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
                common::relay::HandleWrapper{std::move(SocketCodePair.second)}, userHandle.Get()),
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

HRESULT WSLCSession::ListImages(const WSLCListImageOptions* Options, WSLCImageInformation** Images, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Images);
    RETURN_HR_IF_NULL(E_POINTER, Count);

    *Count = 0;
    *Images = nullptr;

    if (Options != nullptr)
    {
        RETURN_HR_IF(E_INVALIDARG, WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsDanglingTrue) && WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsDanglingFalse));
        RETURN_HR_IF(E_INVALIDARG, Options->LabelsCount > 0 && Options->Labels == nullptr);
        RETURN_HR_IF(E_INVALIDARG, Options->Reference != nullptr && strlen(Options->Reference) > WSLC_MAX_IMAGE_NAME_LENGTH);
    }

    auto lock = m_lock.lock_shared();

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    // Extract options for Docker API
    bool all = false;
    bool digests = false;
    DockerHTTPClient::ListImagesFilters filters;

    if (Options != nullptr)
    {
        all = WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsAll);
        digests = WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsDigests);

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
        if (WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsDanglingTrue))
        {
            filters.dangling = true;
        }
        else if (WI_IsFlagSet(Options->Flags, WSLCListImagesFlagsDanglingFalse))
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
                RETURN_HR_IF_NULL(E_POINTER, label.Key);

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

    auto output = wil::make_unique_cotaskmem<WSLCImageInformation[]>(entries);

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

HRESULT WSLCSession::DeleteImage(const WSLCDeleteImageOptions* Options, WSLCDeletedImageInformation** DeletedImages, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Image) > WSLC_MAX_IMAGE_NAME_LENGTH);
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        WI_IsAnyFlagSet(static_cast<WSLCDeleteImageFlags>(Options->Flags), ~WSLCDeleteImageFlagsValid),
        "Invalid flags: 0x%x",
        Options->Flags);
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
            Options->Image, WI_IsFlagSet(Options->Flags, WSLCDeleteImageFlagsForce), WI_IsFlagSet(Options->Flags, WSLCDeleteImageFlagsNoPrune));
    }
    catch (const DockerHTTPException& e)
    {
        std::string errorMessage;
        if ((e.StatusCode() >= 400 && e.StatusCode() < 500))
        {
            errorMessage = e.DockerMessage<docker_schema::ErrorResponse>().message;
        }

        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }

    THROW_HR_IF_MSG(E_FAIL, deletedImages.empty(), "Failed to delete image: %hs", Options->Image);

    auto output = wil::make_unique_cotaskmem<WSLCDeletedImageInformation[]>(deletedImages.size());

    size_t index = 0;
    for (const auto& image : deletedImages)
    {
        THROW_HR_IF(E_UNEXPECTED, (image.Deleted.empty() && image.Untagged.empty()) || (!image.Deleted.empty() && !image.Untagged.empty()));

        if (!image.Deleted.empty())
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Deleted.c_str()) != 0);
            output[index].Type = WSLCDeletedImageTypeDeleted;
        }
        else
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Untagged.c_str()) != 0);
            output[index].Type = WSLCDeletedImageTypeUntagged;
        }

        index++;
    }

    *Count = static_cast<ULONG>(deletedImages.size());
    *DeletedImages = output.release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::TagImage(const WSLCTagImageOptions* Options)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Options->Image);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Image) > WSLC_MAX_IMAGE_NAME_LENGTH);
    RETURN_HR_IF_NULL(E_POINTER, Options->Repo);
    RETURN_HR_IF_NULL(E_POINTER, Options->Tag);
    RETURN_HR_IF(E_INVALIDARG, strlen(Options->Repo) + strlen(Options->Tag) + 1 > WSLC_MAX_IMAGE_NAME_LENGTH);

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
        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::PushImage(LPCSTR Image, LPCSTR RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Image);
    RETURN_HR_IF_NULL(E_POINTER, RegistryAuthenticationInformation);

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto [repo, tagOrDigest] = wslutil::ParseImage(Image);
    auto requestContext = m_dockerClient->PushImage(repo, tagOrDigest, RegistryAuthenticationInformation);
    StreamImageOperation(*requestContext, Image, "Push", ProgressCallback);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::InspectImage(_In_ LPCSTR ImageNameOrId, _Out_ LPSTR* Output)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ImageNameOrId);
    RETURN_HR_IF(E_INVALIDARG, strlen(ImageNameOrId) > WSLC_MAX_IMAGE_NAME_LENGTH);
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

        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), errorMessage, e.StatusCode() == 400);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }

    // Convert to WSLC schema
    auto wslcInspect = ConvertInspectImage(dockerInspect);

    // Serialize to JSON
    std::string wslcJson = wsl::shared::ToJson(wslcInspect);
    *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wslcJson.c_str()).release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::Authenticate(_In_ LPCSTR ServerAddress, _In_ LPCSTR Username, _In_ LPCSTR Password, _Out_ LPSTR* IdentityToken)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, ServerAddress);
    RETURN_HR_IF_NULL(E_POINTER, Username);
    RETURN_HR_IF_NULL(E_POINTER, Password);
    RETURN_HR_IF_NULL(E_POINTER, IdentityToken);

    *IdentityToken = nullptr;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    wil::unique_cotaskmem_ansistring token;

    try
    {
        auto response = m_dockerClient->Authenticate(ServerAddress, Username, Password);
        token = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(response.c_str());
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to authenticate with registry: %hs", ServerAddress);

    *IdentityToken = token.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::PruneImages(const WSLCPruneImagesOptions* Options, WSLCDeletedImageInformation** DeletedImages, ULONG* DeletedImagesCount, ULONGLONG* SpaceReclaimed)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, DeletedImages);
    RETURN_HR_IF_NULL(E_POINTER, DeletedImagesCount);
    RETURN_HR_IF_NULL(E_POINTER, SpaceReclaimed);
    *DeletedImages = nullptr;
    *DeletedImagesCount = 0;
    *SpaceReclaimed = 0;

    if (Options != nullptr)
    {
        RETURN_HR_IF(E_INVALIDARG, WI_IsFlagSet(Options->Flags, WSLCPruneImagesFlagsDanglingTrue) && WI_IsFlagSet(Options->Flags, WSLCPruneImagesFlagsDanglingFalse));
        RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(static_cast<WSLCPruneImagesFlags>(Options->Flags), ~WSLCPruneImagesFlagsValid));
    }

    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    DockerHTTPClient::PruneImagesFilters filters;

    if (Options != nullptr)
    {
        if (WI_IsFlagSet(Options->Flags, WSLCPruneImagesFlagsDanglingTrue))
        {
            filters.dangling = true;
        }
        else if (WI_IsFlagSet(Options->Flags, WSLCPruneImagesFlagsDanglingFalse))
        {
            filters.dangling = false;
        }

        if (Options->Until > 0)
        {
            filters.until = Options->Until;
        }

        if (Options->Labels != nullptr && Options->LabelsCount > 0)
        {
            for (ULONG i = 0; i < Options->LabelsCount; ++i)
            {
                const auto& filter = Options->Labels[i];
                RETURN_HR_IF_NULL(E_POINTER, filter.Key);

                std::string labelFilter = filter.Key;
                if (filter.Value != nullptr)
                {
                    labelFilter += "=";
                    labelFilter += filter.Value;
                }

                if (filter.Present)
                {
                    filters.presentLabels.emplace_back(std::move(labelFilter));
                }
                else
                {
                    filters.absentLabels.emplace_back(std::move(labelFilter));
                }
            }
        }
    }

    docker_schema::PruneImageResult pruneResult;
    try
    {
        pruneResult = m_dockerClient->PruneImages(filters);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to prune images");

    *SpaceReclaimed = pruneResult.SpaceReclaimed;

    if (pruneResult.ImagesDeleted.has_value() && !pruneResult.ImagesDeleted->empty())
    {
        auto output = wil::make_unique_cotaskmem<WSLCDeletedImageInformation[]>(pruneResult.ImagesDeleted->size());
        size_t index = 0;
        for (const auto& image : pruneResult.ImagesDeleted.value())
        {
            THROW_HR_IF(
                E_UNEXPECTED, (image.Deleted.empty() && image.Untagged.empty()) || (!image.Deleted.empty() && !image.Untagged.empty()));

            if (!image.Deleted.empty())
            {
                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Deleted.c_str()) != 0);
                output[index].Type = WSLCDeletedImageTypeDeleted;
            }
            else
            {
                THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, image.Untagged.c_str()) != 0);
                output[index].Type = WSLCDeletedImageTypeUntagged;
            }

            index++;
        }

        *DeletedImages = output.release();
        *DeletedImagesCount = static_cast<ULONG>(pruneResult.ImagesDeleted->size());
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::CreateContainer(const WSLCContainerOptions* containerOptions, IWSLCContainer** Container)
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

    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Image) > WSLC_MAX_IMAGE_NAME_LENGTH);

    // TODO: Log entrance into the function.

    try
    {
        std::scoped_lock lock(m_containersLock, m_volumesLock);

        auto& it = m_containers.emplace_back(WSLCContainerImpl::Create(
            *containerOptions,
            *this,
            m_virtualMachine.value(),
            m_volumes,
            std::bind(&WSLCSession::OnContainerDeleted, this, std::placeholders::_1),
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

        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_IMAGE_NOT_FOUND, errorMessage, e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), errorMessage, e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }
}
CATCH_RETURN();

HRESULT WSLCSession::OpenContainer(LPCSTR Id, IWSLCContainer** Container)
try
{
    COMServiceExecutionContext context;

    ValidateName(Id);

    // Look for an exact ID match first.
    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock{m_containersLock};

    // Purge containers that were auto-deleted via OnEvent (--rm).
    std::erase_if(m_containers, [](const auto& e) { return e->State() == WslcContainerStateDeleted; });
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
            RETURN_HR_IF_MSG(WSLC_E_CONTAINER_PREFIX_AMBIGUOUS, e.StatusCode() == 400, "Ambiguous prefix: '%hs'", Id);

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

HRESULT WSLCSession::ListContainers(WSLCContainerEntry** Containers, ULONG* Count, WSLCContainerPortMapping** Ports, ULONG* PortsCount)
try
{
    COMServiceExecutionContext context;

    *Count = 0;
    *Containers = nullptr;
    *Ports = nullptr;
    *PortsCount = 0;

    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock{m_containersLock};

    // Purge containers that were auto-deleted via OnEvent (--rm).
    std::erase_if(m_containers, [](const auto& e) { return e->State() == WslcContainerStateDeleted; });

    auto output = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(m_containers.size());
    std::vector<WSLCContainerPortMapping> allPorts;

    size_t index = 0;
    for (const auto& e : m_containers)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, e->Image().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Name, e->Name().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Id, e->ID().c_str()) != 0);
        e->GetState(&output[index].State);
        e->GetStateChangedAt(&output[index].StateChangedAt);
        e->GetCreatedAt(&output[index].CreatedAt);

        for (const auto& port : e->GetPorts())
        {
            WSLCContainerPortMapping mapping{};
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(mapping.Id, e->ID().c_str()) != 0);
            mapping.PortMapping.HostPort = port.HostPort;
            mapping.PortMapping.ContainerPort = port.ContainerPort;
            mapping.PortMapping.Family = port.Family;
            mapping.PortMapping.Protocol = port.Protocol;
            THROW_HR_IF(E_UNEXPECTED, port.BindingAddress.size() > WSLC_MAX_BINDING_ADDRESS_LENGTH);
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(mapping.PortMapping.BindingAddress, port.BindingAddress.c_str()) != 0);
            allPorts.push_back(mapping);
        }

        index++;
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = output.release();

    if (!allPorts.empty())
    {
        auto portsOutput = wil::make_unique_cotaskmem<WSLCContainerPortMapping[]>(allPorts.size());
        memcpy(portsOutput.get(), allPorts.data(), allPorts.size() * sizeof(WSLCContainerPortMapping));
        *PortsCount = static_cast<ULONG>(allPorts.size());
        *Ports = portsOutput.release();
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::PruneContainers(_In_opt_ WSLCPruneLabelFilter* Filters, _In_ DWORD FiltersCount, _In_ ULONGLONG Until, _Out_ WSLCPruneContainersResults* Result)
try
{
    COMServiceExecutionContext context;

    DockerHTTPClient::PruneContainersFilters filters;

    if (FiltersCount > 0)
    {
        THROW_HR_IF(E_POINTER, FiltersCount > 0 && Filters == nullptr);

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
                filters.presentLabels.emplace_back(std::move(labelFilter));
            }
            else
            {
                filters.absentLabels.emplace_back(std::move(labelFilter));
            }
        }
    }

    if (Until > 0)
    {
        filters.until = Until;
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

        auto containers = wil::make_unique_cotaskmem<WSLCContainerId[]>(pruneResult.ContainersDeleted->size());

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

HRESULT WSLCSession::CreateRootNamespaceProcess(LPCSTR Executable, const WSLCProcessOptions* Options, IWSLCProcess** Process, int* Errno)
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

void WSLCSession::Ext4Format(const std::string& Device)
{
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, Device});
    auto result = launcher.Launch(*m_virtualMachine).WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());
}

HRESULT WSLCSession::FormatVirtualDisk(LPCWSTR Path)
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

HRESULT WSLCSession::CreateVolume(const WSLCVolumeOptions* Options, WSLCVolumeInformation* VolumeInfo)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, VolumeInfo);
    ZeroMemory(VolumeInfo, sizeof(*VolumeInfo));

    // Default driver to "vhd" if not specified. Currently only "vhd" is supported.
    // TODO: Add support for docker's builtin volume drivers and change the default to "local"
    // to match docker's behaviour.
    std::string driver = (Options->Driver != nullptr) ? Options->Driver : WSLCVhdVolumeDriver;
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcInvalidVolumeType(driver), driver != WSLCVhdVolumeDriver);

    auto driverOpts = wslutil::ParseKeyValuePairs(Options->DriverOpts, Options->DriverOptsCount);
    auto labels = wslutil::ParseKeyValuePairs(Options->Labels, Options->LabelsCount, WSLCVolumeMetadataLabel);

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    std::lock_guard volumesLock(m_volumesLock);

    if (Options->Name != nullptr && Options->Name[0] != '\0')
    {
        ValidateName(Options->Name);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_volumes.contains(Options->Name));
    }

    auto volume = WSLCVhdVolumeImpl::Create(
        Options->Name,
        std::move(driverOpts),
        std::move(labels),
        m_storageVhdPath.parent_path(),
        m_virtualMachine.value(),
        m_dockerClient.value());

    const auto& name = volume->Name();
    auto info = volume->GetVolumeInformation();

    auto [it, inserted] = m_volumes.insert({name, std::move(volume)});
    WI_VERIFY(inserted);

    WSL_LOG("VolumeCreated", TraceLoggingValue(name.c_str(), "VolumeName"));

    *VolumeInfo = info;
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::DeleteVolume(LPCSTR Name)
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
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(name), it == m_volumes.end());

    it->second->Delete();
    m_volumes.erase(it);
    WSL_LOG("VolumeDeleted", TraceLoggingValue(name.c_str(), "VolumeName"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::ListVolumes(WSLCVolumeInformation** Volumes, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Volumes);
    RETURN_HR_IF_NULL(E_POINTER, Count);

    *Volumes = nullptr;
    *Count = 0;

    auto lock = m_lock.lock_shared();
    std::lock_guard volumesLock(m_volumesLock);

    if (m_volumes.empty())
    {
        return S_OK;
    }

    auto output = wil::make_unique_cotaskmem<WSLCVolumeInformation[]>(m_volumes.size());

    ULONG index = 0;
    for (const auto& [name, vol] : m_volumes)
    {
        output[index] = vol->GetVolumeInformation();
        index++;
    }

    *Volumes = output.release();
    *Count = index;

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::InspectVolume(LPCSTR Name, LPSTR* Output)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Name);
    RETURN_HR_IF_NULL(E_POINTER, Output);

    *Output = nullptr;

    std::string name = Name;
    ValidateName(name.c_str());

    auto lock = m_lock.lock_shared();
    std::lock_guard volumesLock(m_volumesLock);

    auto it = m_volumes.find(name);
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(name), it == m_volumes.end());

    const auto& volume = it->second;

    std::string json = volume->Inspect();
    *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(json.c_str()).release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSession::PruneVolumes(const WSLCPruneVolumesOptions* /*Options*/, WSLCPruneVolumesResults* Results)
{
    if (Results != nullptr)
    {
        ZeroMemory(Results, sizeof(*Results));
    }

    // TODO: Implement volume pruning. Docker's volume prune API skips bind-mount volumes,
    // so WSLC VHD volumes require custom handling.
    return E_NOTIMPL;
}

HRESULT WSLCSession::Terminate()
try
{

    {
        std::lock_guard lock(m_userHandlesLock);

        // m_sessionTerminatingEvent is always valid, so it can be signalled without holding m_lock.
        // This allows a session to be unblocked if a stuck operation is holding m_lock.
        // N.B. This must happen under m_userHandlesLock to synchronize with potentially running operations.
        m_sessionTerminatingEvent.SetEvent();

        // Cancel any pending IO on user-provided handles to unblock operations
        // in case the handles don't support overlapped IO.
        CancelUserHandleIO();
    }

    {
        std::lock_guard comLock(m_userCOMCallbacksLock);

        // Cancel any pending outgoing COM callback calls (e.g. IProgressCallback::OnProgress)
        // to unblock operations waiting for cross-process COM responses.
        CancelUserCOMCallbacks();
    }

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
        LOG_IF_FAILED(m_dockerdProcess->Get().Signal(WSLCSignalSIGTERM));

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
                m_dockerdProcess->Get().Signal(WSLCSignalSIGKILL);
                exitCode = m_dockerdProcess->Wait(10 * 1000);
            }
            CATCH_LOG();
        }

        WSL_LOG("DockerdExit", TraceLoggingValue(exitCode, "code"));
        m_dockerdProcess.reset();
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

HRESULT WSLCSession::MountWindowsFolder(LPCWSTR WindowsPath, LPCSTR LinuxPath, BOOL ReadOnly)
try
{
    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}
CATCH_RETURN();

HRESULT WSLCSession::UnmountWindowsFolder(LPCSTR LinuxPath)
try
{
    COMServiceExecutionContext context;

    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->UnmountWindowsFolder(LinuxPath);
}
CATCH_RETURN();

HRESULT WSLCSession::MapVmPort(int Family, unsigned short WindowsPort, unsigned short LinuxPort)
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

HRESULT WSLCSession::UnmapVmPort(int Family, unsigned short WindowsPort, unsigned short LinuxPort)
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

HRESULT WSLCSession::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLCSession) ? S_OK : S_FALSE;
}

MultiHandleWait WSLCSession::CreateIOContext(HANDLE CancelHandle)
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

UserHandle WSLCSession::OpenUserHandle(WSLCHandle Handle)
{
    std::lock_guard lock(m_userHandlesLock);

    // Don't allow new handles to be added to the list if the session is terminating.
    // N.B. This check must happen under m_userHandlesLock to synchronize with Terminate().

    THROW_HR_IF_MSG(
        E_ABORT, m_sessionTerminatingEvent.is_signaled(), "Refusing to open a user handle while the session is terminating.");

    auto userHandle = common::wslutil::FromCOMInputHandle(Handle);

    m_userHandles.emplace_back(userHandle);

    return UserHandle{*this, userHandle};
}

void WSLCSession::ReleaseUserHandle(HANDLE Handle)
{
    std::lock_guard lock(m_userHandlesLock);

    auto it = std::ranges::find(m_userHandles, Handle);
    WI_ASSERT(it != m_userHandles.end());

    m_userHandles.erase(it);
}

void WSLCSession::CancelUserHandleIO()
{
    for (auto handle : m_userHandles)
    {
        // Cancel all IO on the handle.
        // N.B. This only cancels IO happening in this process.
        if (!CancelIoEx(handle, nullptr))
        {
            LOG_LAST_ERROR_IF(GetLastError() != ERROR_NOT_FOUND);
        }
    }
}

UserCOMCallback WSLCSession::RegisterUserCOMCallback()
{
    std::lock_guard lock(m_userCOMCallbacksLock);

    // Don't allow new COM calls if the session is terminating.
    // N.B. This check must happen under m_userCOMCallbacksLock to synchronize with Terminate().
    THROW_HR_IF_MSG(
        E_ABORT, m_sessionTerminatingEvent.is_signaled(), "Refusing to make a COM callback while the session is terminating.");

    THROW_IF_FAILED(CoEnableCallCancellation(nullptr));

    auto [_, inserted] = m_userCOMCallbackThreads.insert(GetCurrentThreadId());
    WI_VERIFY(inserted);

    return UserCOMCallback{*this};
}

void WSLCSession::UnregisterUserCOMCallback(DWORD ThreadId)
{
    std::lock_guard lock(m_userCOMCallbacksLock);

    auto it = m_userCOMCallbackThreads.find(ThreadId);
    WI_VERIFY(it != m_userCOMCallbackThreads.end());

    m_userCOMCallbackThreads.erase(it);
}

void WSLCSession::CancelUserCOMCallbacks()
{
    for (auto threadId : m_userCOMCallbackThreads)
    {
        LOG_IF_FAILED(CoCancelCall(threadId, 0));
    }
}

void WSLCSession::OnContainerDeleted(const WSLCContainerImpl* Container)
{
    auto lock = m_lock.lock_shared();
    std::lock_guard containersLock(m_containersLock);

    WI_VERIFY(std::erase_if(m_containers, [Container](const auto& e) { return e.get() == Container; }) == 1);
}

HRESULT WSLCSession::GetState(_Out_ WSLCSessionState* State)
{
    *State = m_terminated ? WSLCSessionStateTerminated : WSLCSessionStateRunning;
    return S_OK;
}

void WSLCSession::RecoverExistingContainers()
{
    WI_ASSERT(m_dockerClient.has_value());
    WI_ASSERT(m_eventTracker.has_value());
    WI_ASSERT(m_virtualMachine.has_value());

    auto containers = m_dockerClient->ListContainers(true); // all=true to include stopped containers

    for (const auto& dockerContainer : containers)
    {
        try
        {
            auto container = WSLCContainerImpl::Open(
                dockerContainer,
                *this,
                m_virtualMachine.value(),
                m_volumes,
                m_anonymousVolumes,
                std::bind(&WSLCSession::OnContainerDeleted, this, std::placeholders::_1),
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

void WSLCSession::RecoverExistingVolumes()
{
    WI_ASSERT(m_dockerClient.has_value());
    WI_ASSERT(m_virtualMachine.has_value());

    auto volumes = m_dockerClient->ListVolumes();

    std::lock_guard volumesLock(m_volumesLock);

    for (const auto& volume : volumes)
    {
        if (!volume.Labels.has_value() || !volume.Labels->contains(WSLCVolumeMetadataLabel))
        {
            m_anonymousVolumes.insert(volume.Name);
            continue;
        }

        try
        {
            WI_ASSERT(!m_volumes.contains(volume.Name));

            auto vhdVolume = WSLCVhdVolumeImpl::Open(volume, m_virtualMachine.value(), m_dockerClient.value());
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

} // namespace wsl::windows::service::wslc
