/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCComposeTests.cpp

Abstract:

    This file contains test cases for WSLC compose sessions.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslc.h"
#include "wslc_schema.h"
#include "wslutil.h"
#include "WSLCContainerLauncher.h"

using wsl::test::CreateSession;
using wsl::test::GetDefaultWSLCSessionSettings;
using wsl::test::LoadTestImages;
using wsl::windows::common::WSLCContainerLauncher;

extern bool g_fastTestRun;

namespace {

struct ComposeRequestStorage
{
    ComposeRequestStorage(const std::filesystem::path& path, WSLCComposeAction action)
    {
        sourcePath = std::filesystem::absolute(path).lexically_normal();
        baseDirectory = sourcePath.parent_path();
        projectDirectory = baseDirectory;
        workingDirectory = std::filesystem::current_path();

        std::ifstream stream{sourcePath, std::ios::binary};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream);
        content.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});

        document.SourcePath = sourcePath.c_str();
        document.BaseDirectory = baseDirectory.c_str();
        document.Content = reinterpret_cast<const byte*>(content.data());
        document.ContentSize = gsl::narrow<ULONG>(content.size());

        documents.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        documents.WorkingDirectory = workingDirectory.c_str();
        documents.ProjectDirectory = projectDirectory.c_str();
        documents.Documents = &document;
        documents.DocumentCount = 1;

        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        request.Action = action;
        request.Project.Type = WSLCComposeProjectTypeDocuments;
        request.Project.Value.Documents = &documents;
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
    }

    ComposeRequestStorage(std::string projectKeyValue, WSLCComposeAction action) : projectKey(std::move(projectKeyValue))
    {
        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        request.Action = action;
        request.Project.Type = WSLCComposeProjectTypeProjectKey;
        request.Project.Value.ProjectKey = projectKey.c_str();
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
    }

    void SetStopTimeout(ULONG timeout)
    {
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeStop;
        request.ActionOptions.Value.Stop.TimeoutSeconds = timeout;
    }

    void SetProjectName(std::string name)
    {
        explicitProjectName = std::move(name);
        documents.ExplicitProjectName = explicitProjectName.c_str();
    }

    std::filesystem::path sourcePath;
    std::filesystem::path baseDirectory;
    std::filesystem::path projectDirectory;
    std::filesystem::path workingDirectory;
    std::vector<char> content;
    std::string projectKey;
    std::string explicitProjectName;
    WSLCComposeDocument document{};
    WSLCComposeDocuments documents{};
    WSLCComposeOperationRequest request{};
};

struct ComposeResult
{
    NON_COPYABLE(ComposeResult);

    ComposeResult() = default;

    ComposeResult(ComposeResult&& other) noexcept : value(other.value)
    {
        other.value.AffectedContainers = nullptr;
        other.value.AffectedContainersCount = 0;
    }

    ~ComposeResult()
    {
        CoTaskMemFree(value.AffectedContainers);
    }

    WSLCComposeOperationResult value{};
};

class TestComposeProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IComposeProgressCallback, IFastRundown>
{
public:
    explicit TestComposeProgressCallback(bool blockExecuting = false, std::optional<std::string> blockProgress = std::nullopt, bool failOnSuccess = false) :
        m_blockExecuting(blockExecuting), m_blockProgress(std::move(blockProgress)), m_failOnSuccess(failOnSuccess)
    {
        m_blockedEvent.create(wil::EventOptions::ManualReset);
        m_continueEvent.create(wil::EventOptions::ManualReset);
    }

    HRESULT OnProgress(const WSLCComposeProgressEvent* event) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, event);

        bool block = false;
        {
            std::lock_guard lock(m_lock);
            m_orderValid = m_orderValid && event->SequenceNumber == ++m_eventCount;
            switch (event->Kind)
            {
            case WSLCComposeProgressEventKindStatus:
                m_statuses.emplace_back(event->Value.Status.Status);
                RETURN_HR_IF(RPC_E_DISCONNECTED, m_failOnSuccess && event->Value.Status.Status == WSLCComposeStatusSucceeded);
                block = m_blockExecuting && event->Value.Status.Status == WSLCComposeStatusExecuting;
                break;
            case WSLCComposeProgressEventKindProgress:
                m_progress.emplace_back(
                    event->Value.Progress.Operation,
                    event->Value.Progress.ResourceKey,
                    event->Value.Progress.Current,
                    event->Value.Progress.Total,
                    event->Value.Progress.Unit);
                block = m_blockProgress.has_value() && Describe(m_progress.back()) == *m_blockProgress;
                break;
            case WSLCComposeProgressEventKindDiagnostic:
                break;
            default:
                RETURN_HR(E_INVALIDARG);
            }
        }

        if (block)
        {
            m_blockedEvent.SetEvent();
            m_continueEvent.wait();
        }

        return S_OK;
    }
    CATCH_RETURN();

    HRESULT OnStreamsReady(const WSLCComposeStreams*) override
    {
        return S_OK;
    }

    std::vector<WSLCComposeStatus> Statuses() const
    {
        std::lock_guard lock(m_lock);
        return m_statuses;
    }

    struct Progress
    {
        std::string Operation;
        std::string ResourceKey;
        ULONGLONG Current;
        ULONGLONG Total;
        std::string Unit;
    };

    std::vector<Progress> ProgressEvents() const
    {
        std::lock_guard lock(m_lock);
        return m_progress;
    }

    std::vector<std::string> ProgressDescriptions() const
    {
        std::lock_guard lock(m_lock);
        std::vector<std::string> result;
        result.reserve(m_progress.size());
        std::ranges::transform(m_progress, std::back_inserter(result), Describe);
        return result;
    }

    bool OrderValid() const
    {
        std::lock_guard lock(m_lock);
        return m_orderValid;
    }

    void WaitUntilExecuting()
    {
        WaitUntilBlocked();
    }

    void WaitUntilBlocked()
    {
        const DWORD result = WaitForSingleObject(m_blockedEvent.get(), 30'000);
        THROW_LAST_ERROR_IF(result == WAIT_FAILED);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), result == WAIT_TIMEOUT);
        THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0);
    }

    void Continue()
    {
        m_continueEvent.SetEvent();
    }

private:
    static std::string Describe(const Progress& progress)
    {
        return std::format("{}:{}:{}:{}/{}", progress.Unit, progress.Operation, progress.ResourceKey, progress.Current, progress.Total);
    }

    const bool m_blockExecuting;
    const std::optional<std::string> m_blockProgress;
    const bool m_failOnSuccess;
    mutable std::mutex m_lock;
    ULONGLONG m_eventCount{};
    std::vector<WSLCComposeStatus> m_statuses;
    std::vector<Progress> m_progress;
    bool m_orderValid{true};
    wil::unique_event m_blockedEvent;
    wil::unique_event m_continueEvent;
};

ComposeResult WaitForCompose(IWSLCSession& session, ComposeRequestStorage& request, IComposeProgressCallback* progressCallback = nullptr)
{
    wil::com_ptr<IComposeOperation> operation;
    THROW_IF_FAILED(session.BeginComposeOperation(&request.request, progressCallback, &operation));

    wil::unique_handle completionEvent;
    THROW_IF_FAILED(operation->GetCompletionEvent(&completionEvent));
    THROW_LAST_ERROR_IF(WaitForSingleObject(completionEvent.get(), INFINITE) == WAIT_FAILED);

    ComposeResult result;
    THROW_IF_FAILED(operation->GetResult(&result.value));
    return result;
}

ComposeResult ExecuteCompose(IWSLCSession& session, ComposeRequestStorage& request, IComposeProgressCallback* progressCallback = nullptr)
{
    auto result = WaitForCompose(session, request, progressCallback);
    THROW_IF_FAILED(result.value.Result);
    return result;
}

} // namespace

class WSLCComposeTests
{
    WSLC_TEST_CLASS(WSLCComposeTests)

    std::filesystem::path m_storagePath;
    wil::com_ptr<IWSLCSession> m_defaultSession;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        m_storagePath = std::filesystem::current_path() / "test-storage";

        const auto settings = GetDefaultWSLCSessionSettings(L"wslc-compose-test", m_storagePath.c_str(), WSLCNetworkingModeConsomme);
        m_defaultSession = CreateSession(settings);

        LoadTestImages(*m_defaultSession, {"alpine:latest", "python:3.12-alpine"});

        wsl::windows::common::wslutil::PruneResult result;
        VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        wsl::windows::common::wslutil::PruneResult result;
        LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));
        m_defaultSession.reset();

        if (!g_fastTestRun && !m_storagePath.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(m_storagePath, error);
            if (error)
            {
                LogError("Failed to cleanup storage path %ws: %hs", m_storagePath.c_str(), error.message().c_str());
            }
        }

        return true;
    }

    TEST_METHOD(ComposeSessionBasicLifecycle)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-basic-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto volumePath = projectPath / "volume";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directories(volumePath);
        std::ofstream(volumePath / "content.txt") << "compose-volume";

        {
            std::ofstream composeFile(composePath);
            composeFile << "version: \"3.8\"\n"
                           "services:\n"
                           "  basic:\n"
                           "    name: wslc-compose-basic\n"
                           "    image: alpine:latest\n"
                           "    environment:\n"
                           "      COMPOSE_TEST: enabled\n"
                           "    working_dir: /work\n"
                           "    command: [\"/bin/sh\", \"-c\", \"while true; do echo $COMPOSE_TEST; sleep 1; done\"]\n"
                           "    volumes:\n"
                           "      - ./"
                        << volumePath.filename().string()
                        << ":/work:ro\n"
                           "    ports:\n"
                           "      - \"0:8080\"\n"
                           "  secondary:\n"
                           "    name: wslc-compose-secondary\n"
                           "    image: alpine:latest\n"
                           "    command: [\"/bin/sh\", \"-c\", \"while true; do echo secondary; sleep 1; done\"]\n";
        }

        ComposeRequestStorage createRequest{composePath, WSLCComposeActionCreate};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::nullopt, true);
        VERIFY_IS_NOT_NULL(progress.Get());
        const auto createResult = ExecuteCompose(*m_defaultSession, createRequest, progress.Get());
        VERIFY_ARE_EQUAL(2u, createResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(std::string("wslc-compose-basic"), std::string(createResult.value.AffectedContainers[0].Name));
        VERIFY_ARE_EQUAL(WslcContainerStateCreated, createResult.value.AffectedContainers[0].State);
        VERIFY_IS_TRUE(progress->OrderValid());
        VERIFY_ARE_EQUAL(
            std::vector<WSLCComposeStatus>(
                {WSLCComposeStatusValidating, WSLCComposeStatusPlanning, WSLCComposeStatusExecuting, WSLCComposeStatusSucceeded}),
            progress->Statuses());
        VERIFY_ARE_EQUAL(
            std::vector<std::string>(
                {std::format("network:create:{}_default:1/1", projectPath.filename().string()),
                 "container:create:wslc-compose-basic:1/2",
                 "container:create:wslc-compose-secondary:2/2"}),
            progress->ProgressDescriptions());

        wil::com_ptr<IWSLCContainer> basicContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-compose-basic", &basicContainer));
        const auto inspect = InspectContainer(basicContainer.get());
        VERIFY_ARE_EQUAL(std::string("/work"), inspect.Config.WorkingDir);
        VERIFY_IS_TRUE(inspect.Config.Cmd.has_value());
        VERIFY_ARE_EQUAL(
            std::vector<std::string>({"/bin/sh", "-c", "while true; do echo $COMPOSE_TEST; sleep 1; done"}), *inspect.Config.Cmd);
        VERIFY_IS_TRUE(inspect.Config.Env.has_value());
        VERIFY_IS_TRUE(std::ranges::find(*inspect.Config.Env, std::string("COMPOSE_TEST=enabled")) != inspect.Config.Env->end());
        VERIFY_ARE_EQUAL(1u, inspect.Mounts.size());
        VERIFY_ARE_EQUAL(std::string("/work"), inspect.Mounts[0].Destination);
        VERIFY_IS_FALSE(inspect.Mounts[0].ReadWrite);
        VERIFY_IS_TRUE(inspect.Ports.contains("8080/tcp"));
        VERIFY_ARE_EQUAL(1u, inspect.Ports.at("8080/tcp").size());
        VERIFY_ARE_EQUAL(std::string("127.0.0.1"), inspect.Ports.at("8080/tcp")[0].HostIp);

        const std::string basicContainerId = createResult.value.AffectedContainers[0].Id;
        const std::string secondaryContainerId = createResult.value.AffectedContainers[1].Id;

        ComposeRequestStorage startRequest{composePath, WSLCComposeActionStart};
        auto startProgress = Microsoft::WRL::Make<TestComposeProgressCallback>();
        VERIFY_IS_NOT_NULL(startProgress.Get());
        const auto startResult = ExecuteCompose(*m_defaultSession, startRequest, startProgress.Get());
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, startResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, startResult.value.AffectedContainers[1].State);
        VERIFY_ARE_EQUAL(basicContainerId, std::string(startResult.value.AffectedContainers[0].Id));
        VERIFY_ARE_EQUAL(secondaryContainerId, std::string(startResult.value.AffectedContainers[1].Id));
        VERIFY_ARE_EQUAL(
            std::vector<std::string>({"container:start:wslc-compose-basic:1/2", "container:start:wslc-compose-secondary:2/2"}),
            startProgress->ProgressDescriptions());

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        auto upProgress = Microsoft::WRL::Make<TestComposeProgressCallback>();
        VERIFY_IS_NOT_NULL(upProgress.Get());
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest, upProgress.Get());
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, upResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, upResult.value.AffectedContainers[1].State);
        VERIFY_ARE_NOT_EQUAL(basicContainerId, std::string(upResult.value.AffectedContainers[0].Id));
        VERIFY_ARE_NOT_EQUAL(secondaryContainerId, std::string(upResult.value.AffectedContainers[1].Id));
        VERIFY_ARE_EQUAL(
            std::vector<std::string>(
                {"container:remove:wslc-compose-basic:1/2",
                 "container:remove:wslc-compose-secondary:2/2",
                 std::format("network:remove:{}_default:1/1", projectPath.filename().string()),
                 std::format("network:create:{}_default:1/1", projectPath.filename().string()),
                 "container:create:wslc-compose-basic:1/2",
                 "container:create:wslc-compose-secondary:2/2",
                 "container:start:wslc-compose-basic:1/2",
                 "container:start:wslc-compose-secondary:2/2"}),
            upProgress->ProgressDescriptions());

        ComposeRequestStorage attachRequest{composePath, WSLCComposeActionAttach};
        const auto attachResult = ExecuteCompose(*m_defaultSession, attachRequest);
        VERIFY_ARE_EQUAL(2u, attachResult.value.AffectedContainersCount);

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(10);
        auto stopProgress = Microsoft::WRL::Make<TestComposeProgressCallback>();
        VERIFY_IS_NOT_NULL(stopProgress.Get());
        const auto stopResult = ExecuteCompose(*m_defaultSession, stopRequest, stopProgress.Get());
        VERIFY_ARE_EQUAL(2u, stopResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, stopResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, stopResult.value.AffectedContainers[1].State);
        VERIFY_ARE_EQUAL(
            std::vector<std::string>({"container:stop:wslc-compose-basic:1/2", "container:stop:wslc-compose-secondary:2/2"}),
            stopProgress->ProgressDescriptions());

        for (const auto& name : {"wslc-compose-basic", "wslc-compose-secondary"})
        {
            wil::com_ptr<IWSLCContainer> container;
            VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(name, &container));
            VERIFY_SUCCEEDED(container->Delete(WSLCDeleteFlagsNone));
        }
        VERIFY_SUCCEEDED(m_defaultSession->DeleteNetwork(std::format("{}_default", projectPath.filename().string()).c_str()));
    }

    TEST_METHOD(ComposeEmptyServiceNameValidatedBeforeMutation)
    {
        VerifyInvalidServiceNameValidatedBeforeMutation("", false);
    }

    TEST_METHOD(ComposeEmptyServiceNameWithExplicitContainerNameValidatedBeforeMutation)
    {
        VerifyInvalidServiceNameValidatedBeforeMutation("", true);
    }

    TEST_METHOD(ComposeWhitespaceServiceNameValidatedBeforeMutation)
    {
        VerifyInvalidServiceNameValidatedBeforeMutation(" \t\r\n ", true);
    }

    TEST_METHOD(ComposeLeadingNulServiceNameValidatedBeforeMutation)
    {
        constexpr char c_serviceName[] = "\0service";
        VerifyInvalidServiceNameValidatedBeforeMutation({c_serviceName, std::size(c_serviceName) - 1}, true);
    }

    TEST_METHOD(ComposeEmbeddedNulServiceNameValidatedBeforeMutation)
    {
        constexpr char c_serviceName[] = "service\0suffix";
        VerifyInvalidServiceNameValidatedBeforeMutation({c_serviceName, std::size(c_serviceName) - 1}, true);
    }

    TEST_METHOD(ComposeDuplicateResolvedContainerNamesValidatedBeforeMutation)
    {
        const auto projectName = std::format("compose-resolved-names-{}", GetCurrentProcessId());
        const auto content = std::format(
            "services:\n"
            "  web:\n"
            "    image: alpine:latest\n"
            "  other:\n"
            "    image: alpine:latest\n"
            "    container_name: {}-web-1\n",
            projectName);
        VerifyComposeRejectedBeforeMutation(projectName, content);
    }

    TEST_METHOD(ComposeDuplicateExplicitContainerNamesValidatedBeforeMutation)
    {
        const auto projectName = std::format("compose-explicit-names-{}", GetCurrentProcessId());
        const auto content = std::format(
            "services:\n"
            "  first:\n"
            "    image: alpine:latest\n"
            "    container_name: {0}-shared\n"
            "  second:\n"
            "    image: alpine:latest\n"
            "    container_name: {0}-shared\n",
            projectName);
        VerifyComposeRejectedBeforeMutation(projectName, content);
    }

    TEST_METHOD(ComposeNulContainerNamesValidatedBeforeMutation)
    {
        const auto projectName = std::format("compose-nul-names-{}", GetCurrentProcessId());
        const auto content = std::format(
            "services:\n"
            "  first:\n"
            "    image: alpine:latest\n"
            "    container_name: \"{0}-shared\\0a\"\n"
            "  second:\n"
            "    image: alpine:latest\n"
            "    container_name: \"{0}-shared\\0b\"\n",
            projectName);
        VerifyComposeRejectedBeforeMutation(projectName, content);
    }

    TEST_METHOD(ComposeUtf16LittleEndianDocument)
    {
        VerifyUtf16ComposeDocument(false);
    }

    TEST_METHOD(ComposeUtf16BigEndianDocument)
    {
        VerifyUtf16ComposeDocument(true);
    }

    TEST_METHOD(ComposeSessionFileSwitchRejectedBeforeMutation)
    {
        VerifyProjectFileSwitchRejectedBeforeMutation(false);
    }

    TEST_METHOD(ComposeSessionNormalizedProjectCollisionRejectedBeforeMutation)
    {
        VerifyProjectFileSwitchRejectedBeforeMutation(true);
    }

    TEST_METHOD(ComposeContainerNameHttpRequest)
    {
        VerifyComposeHttpRequest(false);
    }

    TEST_METHOD(ComposeServiceNameHttpRequest)
    {
        VerifyComposeHttpRequest(true);
    }

    void VerifyComposeHttpRequest(bool defaultNames)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-http-{}-{}", defaultNames, GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        {
            std::ofstream composeFile(composePath);
            composeFile << "services:\n"
                           "  server:\n";
            if (!defaultNames)
            {
                composeFile << "    name: wslc-compose-http-server\n";
            }
            composeFile << "    image: python:3.12-alpine\n"
                           "    command: [\"python3\", \"-m\", \"http.server\", \"8000\"]\n"
                           "  client:\n";
            if (!defaultNames)
            {
                composeFile << "    name: wslc-compose-http-client\n";
            }
            composeFile
                << "    image: python:3.12-alpine\n"
                   "    command: [\"python3\", \"-c\", \"import time, urllib.request\\nwhile True:\\n    try:\\n        with "
                   "urllib.request.urlopen(\\\"http://"
                << (defaultNames ? "server" : "wslc-compose-http-server")
                << ":8000/\\\", timeout=10) as response:\\n            "
                   "if response.status == 200:\\n                break\\n    except Exception:\\n        pass\\n    "
                   "time.sleep(1)\"]\n";
        }

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);
        const std::string projectKey = upResult.value.ProjectKey;
        auto stopCompose = wil::scope_exit([&] {
            LOG_IF_FAILED(wil::ResultFromException([&]() {
                ComposeRequestStorage stopRequest{projectKey, WSLCComposeActionStop};
                stopRequest.SetStopTimeout(0);
                ExecuteCompose(*m_defaultSession, stopRequest);
            }));
        });

        VERIFY_ARE_EQUAL(2u, upResult.value.AffectedContainersCount);
        if (defaultNames)
        {
            const std::vector<std::string> expectedNames{std::format("{}-client-1", projectKey), std::format("{}-server-1", projectKey)};
            std::vector<std::string> actualNames{upResult.value.AffectedContainers[0].Name, upResult.value.AffectedContainers[1].Name};
            std::ranges::sort(actualNames);
            VERIFY_ARE_EQUAL(expectedNames, actualNames);
        }

        wil::com_ptr<IWSLCContainer> clientContainer;
        const auto clientName = defaultNames ? std::format("{}-client-1", projectKey) : std::string{"wslc-compose-http-client"};
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(clientName.c_str(), &clientContainer));
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                const auto inspect = InspectContainer(clientContainer.get());
                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), inspect.State.Status != "exited");
                THROW_HR_IF(E_FAIL, inspect.State.ExitCode != 0);
            },
            100ms,
            120s);
    }

    TEST_METHOD(ComposeOperationCancellation)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-cancel-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        std::ofstream(composePath) << R"(
services:
  cancelled:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)";

        ComposeRequestStorage request{composePath, WSLCComposeActionUp};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        wil::com_ptr<IComposeOperation> operation;
        VERIFY_SUCCEEDED(m_defaultSession->BeginComposeOperation(&request.request, progress.Get(), &operation));
        auto ensureUnblocked = wil::scope_exit([&] {
            LOG_IF_FAILED(operation->Cancel());
            progress->Continue();
        });

        progress->WaitUntilExecuting();
        VERIFY_SUCCEEDED(operation->Cancel());
        progress->Continue();

        wil::unique_handle completionEvent;
        VERIFY_SUCCEEDED(operation->GetCompletionEvent(&completionEvent));
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(completionEvent.get(), 30'000));

        ComposeResult result;
        VERIFY_SUCCEEDED(operation->GetResult(&result.value));
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(projectPath.filename().string(), std::string{result.value.ProjectKey});
        VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        VERIFY_IS_TRUE(progress->OrderValid());
        VERIFY_ARE_EQUAL(WSLCComposeStatusCancelled, progress->Statuses().back());
    }

    TEST_METHOD(ComposeLateCancellationDoesNotOverrideCompletedMutation)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-late-cancel-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto markerPath = projectPath / "term-received";
        const auto projectKey = projectPath.filename().string();
        const auto containerName = std::format("{}-late-cancel-1", projectKey);
        auto cleanup = wil::scope_exit([&] {
            wil::com_ptr<IWSLCContainer> container;
            if (SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container)))
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(std::format("{}_default", projectKey).c_str()));
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        {
            std::ofstream composeFile(composePath);
            composeFile << R"(
services:
  late-cancel:
    image: alpine:latest
    command: ["/bin/sh", "-c", "trap 'touch /work/term-received; sleep 30' TERM; while true; do sleep 1; done"]
    volumes:
      - ./:/work
)";
        }

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(5);
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        wil::com_ptr<IComposeOperation> operation;
        VERIFY_SUCCEEDED(m_defaultSession->BeginComposeOperation(&stopRequest.request, progress.Get(), &operation));
        auto ensureUnblocked = wil::scope_exit([&] {
            progress->Continue();
            LOG_IF_FAILED(operation->Cancel());
        });

        progress->WaitUntilExecuting();
        progress->Continue();

        wil::unique_handle completionEvent;
        VERIFY_SUCCEEDED(operation->GetCompletionEvent(&completionEvent));
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(markerPath)); }, 100ms, 30s);
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(completionEvent.get(), 0));
        VERIFY_SUCCEEDED(operation->Cancel());

        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(completionEvent.get(), 30'000));

        ComposeResult result;
        VERIFY_SUCCEEDED(operation->GetResult(&result.value));
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusSucceeded, result.value.Status);
        VERIFY_SUCCEEDED(result.value.Result);
        VERIFY_ARE_EQUAL(std::string{upResult.value.ProjectKey}, std::string{result.value.ProjectKey});
        VERIFY_ARE_EQUAL(1u, result.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, result.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WSLCComposeStatusSucceeded, progress->Statuses().back());
    }

    TEST_METHOD(ComposeCancellationAfterMutationLeavesDiscoverablePartialState)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-partial-cancel-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto markerPath = projectPath / "term-received";
        const auto projectKey = projectPath.filename().string();
        const auto firstContainerName = std::format("{}-first-1", projectKey);
        const auto secondContainerName = std::format("{}-second-1", projectKey);
        auto cleanup = wil::scope_exit([&] {
            for (const auto& name : {firstContainerName, secondContainerName})
            {
                wil::com_ptr<IWSLCContainer> container;
                if (SUCCEEDED(m_defaultSession->OpenContainer(name.c_str(), &container)))
                {
                    LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
                }
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(std::format("{}_default", projectKey).c_str()));
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        std::ofstream(composePath) << R"(
services:
  first:
    image: alpine:latest
    command: ["/bin/sh", "-c", "trap 'touch /work/term-received; sleep 30' TERM; while true; do sleep 1; done"]
    volumes:
      - ./:/work
  second:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)";

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(5);
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        wil::com_ptr<IComposeOperation> operation;
        VERIFY_SUCCEEDED(m_defaultSession->BeginComposeOperation(&stopRequest.request, progress.Get(), &operation));
        auto ensureUnblocked = wil::scope_exit([&] {
            progress->Continue();
            LOG_IF_FAILED(operation->Cancel());
        });

        progress->WaitUntilExecuting();
        progress->Continue();

        wil::unique_handle completionEvent;
        VERIFY_SUCCEEDED(operation->GetCompletionEvent(&completionEvent));
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(markerPath)); }, 100ms, 30s);
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(completionEvent.get(), 0));
        VERIFY_SUCCEEDED(operation->Cancel());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(completionEvent.get(), 30'000));

        ComposeResult result;
        VERIFY_SUCCEEDED(operation->GetResult(&result.value));
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(std::string{upResult.value.ProjectKey}, std::string{result.value.ProjectKey});

        wil::com_ptr<IWSLCContainer> firstContainer;
        wil::com_ptr<IWSLCContainer> secondContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(firstContainerName.c_str(), &firstContainer));
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(secondContainerName.c_str(), &secondContainer));

        WSLCContainerState firstState{};
        WSLCContainerState secondState{};
        VERIFY_SUCCEEDED(firstContainer->GetState(&firstState));
        VERIFY_SUCCEEDED(secondContainer->GetState(&secondState));
        VERIFY_ARE_EQUAL(WslcContainerStateExited, firstState);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, secondState);

        const auto retryResult = ExecuteCompose(*m_defaultSession, stopRequest);
        VERIFY_ARE_EQUAL(2u, retryResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, retryResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, retryResult.value.AffectedContainers[1].State);
    }

    TEST_METHOD(ComposeCancellationDuringCreatePreservesCompletedResources)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-create-cancel-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto projectKey = projectPath.filename().string();
        const auto networkName = std::format("{}_default", projectKey);
        const auto firstContainerName = std::format("{}-first-1", projectKey);
        const auto secondContainerName = std::format("{}-second-1", projectKey);
        auto cleanup = wil::scope_exit([&] {
            for (const auto& name : {firstContainerName, secondContainerName})
            {
                wil::com_ptr<IWSLCContainer> container;
                if (SUCCEEDED(m_defaultSession->OpenContainer(name.c_str(), &container)))
                {
                    LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
                }
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        std::ofstream(composePath) << R"(
services:
  first:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
  second:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)";

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        auto progress =
            Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::format("container:create:{}:2/2", secondContainerName));
        VERIFY_IS_NOT_NULL(progress.Get());

        wil::com_ptr<IComposeOperation> operation;
        VERIFY_SUCCEEDED(m_defaultSession->BeginComposeOperation(&upRequest.request, progress.Get(), &operation));
        auto ensureUnblocked = wil::scope_exit([&] {
            LOG_IF_FAILED(operation->Cancel());
            progress->Continue();
        });

        progress->WaitUntilBlocked();
        VERIFY_SUCCEEDED(operation->Cancel());
        progress->Continue();

        wil::unique_handle completionEvent;
        VERIFY_SUCCEEDED(operation->GetCompletionEvent(&completionEvent));
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(completionEvent.get(), 30'000));

        ComposeResult result;
        VERIFY_SUCCEEDED(operation->GetResult(&result.value));
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(projectKey, std::string{result.value.ProjectKey});

        wil::unique_cotaskmem_ansistring networkInspect;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &networkInspect));

        wil::com_ptr<IWSLCContainer> firstContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(firstContainerName.c_str(), &firstContainer));
        WSLCContainerState firstState{};
        VERIFY_SUCCEEDED(firstContainer->GetState(&firstState));
        VERIFY_ARE_EQUAL(WslcContainerStateCreated, firstState);

        wil::com_ptr<IWSLCContainer> secondContainer;
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(secondContainerName.c_str(), &secondContainer));

        ComposeRequestStorage retryRequest{composePath, WSLCComposeActionUp};
        const auto retryResult = ExecuteCompose(*m_defaultSession, retryRequest);
        VERIFY_ARE_EQUAL(2u, retryResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, retryResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, retryResult.value.AffectedContainers[1].State);
    }

    TEST_METHOD(ComposeDerivedNameLengthsValidatedBeforeMutation)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-name-length-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);

        // Generated names are "<project>_default" for the network and "<project>-<service>-1" for containers.
        constexpr size_t c_networkSuffixLength = std::string_view{"_default"}.size();
        constexpr size_t c_containerAffixLength = std::string_view{"--1"}.size();

        const auto runCase = [&](std::string_view projectName, std::string_view serviceName, WSLCComposeAction action) {
            std::ofstream(composePath, std::ios::trunc) << std::format(
                R"(
services:
  {}:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)",
                serviceName);

            ComposeRequestStorage request{composePath, action};
            request.SetProjectName(std::string{projectName});
            auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>();
            THROW_IF_NULL_ALLOC(progress.Get());
            auto result = WaitForCompose(*m_defaultSession, request, progress.Get());
            return std::make_pair(std::move(result), progress->Statuses());
        };

        const std::vector expectedFailureStatuses{WSLCComposeStatusValidating, WSLCComposeStatusFailed};

        {
            const std::string projectName(WSLC_MAX_NETWORK_NAME_LENGTH - c_networkSuffixLength, 'p');
            const auto [result, statuses] = runCase(projectName, "s", WSLCComposeActionValidate);
            VERIFY_SUCCEEDED(result.value.Result);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusSucceeded, result.value.Status);
            VERIFY_ARE_EQUAL(projectName, std::string{result.value.ProjectKey});
        }

        {
            const std::string projectName(WSLC_MAX_NETWORK_NAME_LENGTH - c_networkSuffixLength + 1, 'p');
            const auto [result, statuses] = runCase(projectName, "s", WSLCComposeActionUp);
            VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_IS_TRUE(statuses == expectedFailureStatuses);
        }

        {
            const std::string projectName{"p"};
            const std::string serviceName(WSLC_MAX_CONTAINER_NAME_LENGTH - c_containerAffixLength - projectName.size(), 's');
            const auto [result, statuses] = runCase(projectName, serviceName, WSLCComposeActionValidate);
            VERIFY_SUCCEEDED(result.value.Result);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusSucceeded, result.value.Status);
        }

        {
            const std::string projectName{"p"};
            const std::string serviceName(WSLC_MAX_CONTAINER_NAME_LENGTH - c_containerAffixLength - projectName.size() + 1, 's');
            const auto [result, statuses] = runCase(projectName, serviceName, WSLCComposeActionUp);
            VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_IS_TRUE(statuses == expectedFailureStatuses);
        }
    }

    TEST_METHOD(ComposeLifecycleDiscoversManagedContainersByLabel)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-discovery-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto projectKey = projectPath.filename().string();
        const auto containerName = std::format("{}-external", projectKey);
        const auto composeContainerName = std::format("{}-service-1", projectKey);
        auto cleanup = wil::scope_exit([&] {
            for (const auto& name : {containerName, composeContainerName})
            {
                wil::com_ptr<IWSLCContainer> container;
                if (SUCCEEDED(m_defaultSession->OpenContainer(name.c_str(), &container)))
                {
                    LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
                }
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(std::format("{}_default", projectKey).c_str()));
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        std::ofstream(composePath) << R"(
services:
  service:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)";

        WSLCContainerLauncher launcher("alpine:latest", containerName, {"/bin/sh", "-c", "while true; do sleep 1; done"});
        launcher.AddLabel("com.docker.compose.project", projectKey);
        launcher.AddLabel("com.docker.compose.service", "service");
        launcher.AddLabel("com.docker.compose.container-number", "1");
        launcher.AddLabel("com.docker.compose.oneoff", "False");
        launcher.AddLabel("com.microsoft.wslc.compose.managed", "true");
        launcher.AddLabel("com.microsoft.wslc.compose.metadata-version", "1");
        auto existingContainer = launcher.Create(*m_defaultSession);
        existingContainer.SetDeleteOnClose(false);
        const auto existingId = existingContainer.Id();

        ComposeRequestStorage startRequest{projectKey, WSLCComposeActionStart};
        const auto startResult = ExecuteCompose(*m_defaultSession, startRequest);
        VERIFY_ARE_EQUAL(1u, startResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(existingId, std::string{startResult.value.AffectedContainers[0].Id});
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, startResult.value.AffectedContainers[0].State);

        ComposeRequestStorage attachRequest{projectKey, WSLCComposeActionAttach};
        const auto attachResult = ExecuteCompose(*m_defaultSession, attachRequest);
        VERIFY_ARE_EQUAL(1u, attachResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(existingId, std::string{attachResult.value.AffectedContainers[0].Id});

        ComposeRequestStorage stopRequest{projectKey, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(0);
        const auto stopResult = ExecuteCompose(*m_defaultSession, stopRequest);
        VERIFY_ARE_EQUAL(1u, stopResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(existingId, std::string{stopResult.value.AffectedContainers[0].Id});
        VERIFY_ARE_EQUAL(WslcContainerStateExited, stopResult.value.AffectedContainers[0].State);

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);
        VERIFY_ARE_EQUAL(1u, upResult.value.AffectedContainersCount);
        VERIFY_ARE_NOT_EQUAL(existingId, std::string{upResult.value.AffectedContainers[0].Id});
        VERIFY_ARE_EQUAL(composeContainerName, std::string{upResult.value.AffectedContainers[0].Name});
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, upResult.value.AffectedContainers[0].State);
    }

    TEST_METHOD(ComposeDiscoveryValidatesAndSortsContainerNumbers)
    {
        const auto projectKey = std::format("compose-discovery-order-{}", GetCurrentProcessId());
        std::vector<wsl::windows::common::RunningWSLCContainer> containers;
        std::map<uint64_t, std::string> containerIds;

        const auto createContainer = [&](std::string_view suffix, std::string_view containerNumber) {
            WSLCContainerLauncher launcher(
                "alpine:latest", std::format("{}-{}", projectKey, suffix), {"/bin/sh", "-c", "while true; do sleep 1; done"});
            launcher.AddLabel("com.docker.compose.project", projectKey);
            launcher.AddLabel("com.docker.compose.service", "service");
            launcher.AddLabel("com.docker.compose.container-number", std::string{containerNumber});
            launcher.AddLabel("com.docker.compose.oneoff", "False");
            launcher.AddLabel("com.microsoft.wslc.compose.managed", "true");
            launcher.AddLabel("com.microsoft.wslc.compose.metadata-version", "1");
            containers.emplace_back(launcher.Create(*m_defaultSession));
        };

        for (const auto number : {10ull, 2ull, 1ull})
        {
            createContainer(std::to_string(number), std::to_string(number));
            containerIds.emplace(number, containers.back().Id());
        }

        ComposeRequestStorage attachRequest{projectKey, WSLCComposeActionAttach};
        const auto attachResult = ExecuteCompose(*m_defaultSession, attachRequest);
        VERIFY_ARE_EQUAL(3u, attachResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(containerIds.at(1), std::string{attachResult.value.AffectedContainers[0].Id});
        VERIFY_ARE_EQUAL(containerIds.at(2), std::string{attachResult.value.AffectedContainers[1].Id});
        VERIFY_ARE_EQUAL(containerIds.at(10), std::string{attachResult.value.AffectedContainers[2].Id});

        createContainer("invalid", "not-a-number");
        const auto invalidResult = WaitForCompose(*m_defaultSession, attachRequest);
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, invalidResult.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), invalidResult.value.Result);
        VERIFY_ARE_EQUAL(0u, invalidResult.value.AffectedContainersCount);
    }

    TEST_METHOD(ComposeUnsupportedReferencesFailBeforeMutation)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-invalid-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto containerName = std::format("wslc-compose-invalid-{}", GetCurrentProcessId());
        auto cleanup = wil::scope_exit([&] {
            wil::com_ptr<IWSLCContainer> container;
            if (SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container)))
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            const auto networkName = projectPath.filename().string() + "_default";
            const auto deleteNetworkResult = m_defaultSession->DeleteNetwork(networkName.c_str());
            if (deleteNetworkResult != WSLC_E_NETWORK_NOT_FOUND)
            {
                LOG_IF_FAILED(deleteNetworkResult);
            }

            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        const auto service = std::format(
            R"(
services:
  invalid:
    name: {}
    image: alpine:latest
)",
            containerName);
        const std::array unsupportedDocuments{
            std::format(
                R"(
include:
  - other.yaml
{})",
                service),
            std::format(
                R"(
services:
  invalid:
    name: {}
    image: alpine:latest
    env_file: .env
)",
                containerName),
            std::format(
                R"(
services:
  invalid:
    name: {}
    image: alpine:latest
    label_file: labels.txt
)",
                containerName),
            std::format(
                R"(
services:
  invalid:
    name: {}
    image: alpine:latest
    extends:
      file: common.yaml
      service: common
)",
                containerName),
            std::format(
                R"(
configs:
  settings:
    file: settings.conf
{})",
                service),
            std::format(
                R"(
secrets:
  token:
    file: token.txt
{})",
                service),
        };

        for (const auto& document : unsupportedDocuments)
        {
            std::ofstream(composePath) << document;
            ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
            const auto result = WaitForCompose(*m_defaultSession, request);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
            VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        }

        wil::com_ptr<IWSLCContainer> container;
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(containerName.c_str(), &container));
    }

    TEST_METHOD(ComposeUnsupportedSelectionFails)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-selection-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        std::ofstream(composePath) << R"(
services:
  selected:
    image: alpine:latest
)";

        LPCSTR selectionValues[] = {"selected"};
        const auto verifyRejected = [&](ComposeRequestStorage& request) {
            const auto result = WaitForCompose(*m_defaultSession, request);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_ARE_EQUAL(E_NOTIMPL, result.value.Result);
            VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        };

        ComposeRequestStorage profilesRequest{composePath, WSLCComposeActionValidate};
        profilesRequest.request.Selection.Profiles = {.Values = selectionValues, .Count = static_cast<ULONG>(ARRAYSIZE(selectionValues))};
        verifyRejected(profilesRequest);

        ComposeRequestStorage servicesRequest{composePath, WSLCComposeActionValidate};
        servicesRequest.request.Selection.Services = {.Values = selectionValues, .Count = static_cast<ULONG>(ARRAYSIZE(selectionValues))};
        verifyRejected(servicesRequest);

        ComposeRequestStorage dependenciesRequest{composePath, WSLCComposeActionValidate};
        dependenciesRequest.request.Selection.IncludeDependencies = TRUE;
        verifyRejected(dependenciesRequest);

        ComposeRequestStorage projectKeyRequest{std::string{"missing-project"}, WSLCComposeActionStart};
        projectKeyRequest.request.Selection.IncludeDependencies = TRUE;
        verifyRejected(projectKeyRequest);
    }

private:
    void VerifyInvalidServiceNameValidatedBeforeMutation(std::string_view serviceName, bool useExplicitContainerName)
    {
        const auto projectName = std::format("compose-service-name-{}-{}", useExplicitContainerName, GetCurrentProcessId());
        auto content = std::format("services:\n  {}:\n    image: alpine:latest\n", nlohmann::json(std::string{serviceName}).dump());
        if (useExplicitContainerName)
        {
            content += std::format("    container_name: {}-container\n", projectName);
        }

        VerifyComposeRejectedBeforeMutation(projectName, content);
    }

    void VerifyComposeRejectedBeforeMutation(const std::string& projectName, std::string_view content)
    {
        const auto projectPath = std::filesystem::current_path() / projectName;
        const auto composePath = projectPath / "compose.yaml";
        const auto networkName = projectName + "_default";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
            if (error)
            {
                LogError("Failed to cleanup project path %ws: %hs", projectPath.c_str(), error.message().c_str());
            }
        });

        WriteComposeFile(composePath, content);

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));
        auto networkCleanup = wil::scope_exit([&] { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        wil::unique_cotaskmem_ansistring originalOutput;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &originalOutput));
        const auto originalNetwork = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(originalOutput.get());
        VERIFY_IS_FALSE(originalNetwork.Id.empty());

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        VERIFY_IS_NULL(composeSession.get());

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(originalNetwork.Id, network.Id);
    }

    void VerifyUtf16ComposeDocument(bool bigEndian)
    {
        const auto projectName = std::format("compose-utf16-{}-{}", bigEndian, GetCurrentProcessId());
        const auto projectPath = std::filesystem::current_path() / projectName;
        const auto composePath = projectPath / "compose.yaml";
        const auto networkName = projectName + "_default";
        const auto containerName = projectName + "-encoded-1";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
            if (error)
            {
                LogError("Failed to cleanup project path %ws: %hs", projectPath.c_str(), error.message().c_str());
            }
        });

        constexpr std::u16string_view c_yaml =
            u"services:\n"
            u"  encoded:\n"
            u"    image: alpine:latest\n"
            u"    environment:\n"
            u"      ENCODING_TEST: \u00e9\n";
        std::string content = bigEndian ? std::string{"\xfe\xff", 2} : std::string{"\xff\xfe", 2};
        for (const auto value : c_yaml)
        {
            const auto low = static_cast<char>(value & 0xff);
            const auto high = static_cast<char>(value >> 8);
            content.push_back(bigEndian ? high : low);
            content.push_back(bigEndian ? low : high);
        }

        WriteComposeFile(composePath, content);

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        wil::com_ptr<IWSLCContainer> container;
        auto resourceCleanup = wil::scope_exit([&] {
            if (container)
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        });

        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container));
        const auto inspect = InspectContainer(container.get());
        VERIFY_ARE_EQUAL(std::string{"encoded"}, inspect.Config.Labels.at("com.docker.compose.service"));
        VERIFY_IS_TRUE(inspect.Config.Env.has_value());
        VERIFY_IS_TRUE(std::ranges::find(*inspect.Config.Env, std::string{"ENCODING_TEST=\xc3\xa9"}) != inspect.Config.Env->end());
    }

    void VerifyProjectFileSwitchRejectedBeforeMutation(bool differentDirectory)
    {
        const auto projectName = std::format("compose-project-files-{}-{}", differentDirectory, GetCurrentProcessId());
        const auto sourceRoot = std::filesystem::current_path() / (projectName + "-source");
        const auto projectPath = sourceRoot / projectName;
        const auto alternateProjectPath = differentDirectory ? sourceRoot / (projectName + "!") : projectPath;
        const auto composePath = projectPath / "compose.yaml";
        const auto alternateComposePath = alternateProjectPath / (differentDirectory ? "compose.yaml" : "alternate.yaml");
        const auto networkName = projectName + "_default";
        const std::array containerNames{projectName + "-first-1", projectName + "-second-1"};

        VERIFY_IS_TRUE(std::filesystem::create_directory(sourceRoot));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(sourceRoot, error);
            if (error)
            {
                LogError("Failed to cleanup project source path %ws: %hs", sourceRoot.c_str(), error.message().c_str());
            }
        });

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        if (differentDirectory)
        {
            VERIFY_IS_TRUE(std::filesystem::create_directory(alternateProjectPath));
        }

        WriteComposeFile(composePath, "services:\n  first:\n    image: alpine:latest\n");
        WriteComposeFile(alternateComposePath, "services:\n  second:\n    image: alpine:latest\n");

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        auto resourceCleanup = wil::scope_exit([&] {
            for (const auto& name : containerNames)
            {
                wil::com_ptr<IWSLCContainer> container;
                const auto result = m_defaultSession->OpenContainer(name.c_str(), &container);
                if (result == WSLC_E_CONTAINER_NOT_FOUND)
                {
                    continue;
                }

                LOG_IF_FAILED(result);
                if (container)
                {
                    LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
                }
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        });

        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
        VERIFY_SUCCEEDED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, containers.size());
        const std::string originalContainerId = containers[0].Id;

        wil::com_ptr<IWSLCComposeSession> reusedSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &reusedSession));
        containers.reset();
        VERIFY_SUCCEEDED(reusedSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, containers.size());
        VERIFY_ARE_EQUAL(originalContainerId, std::string{containers[0].Id});

        wil::unique_cotaskmem_ansistring originalOutput;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &originalOutput));
        const auto originalNetwork = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(originalOutput.get());
        VERIFY_IS_FALSE(originalNetwork.Id.empty());

        wil::com_ptr<IWSLCContainer> firstContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerNames[0].c_str(), &firstContainer));
        VERIFY_SUCCEEDED(firstContainer->Delete(WSLCDeleteFlagsForce));

        wil::com_ptr<IWSLCComposeSession> otherSession;
        VERIFY_ARE_EQUAL(E_NOTIMPL, m_defaultSession->CreateComposeSession(alternateComposePath.c_str(), &otherSession));
        VERIFY_IS_NULL(otherSession.get());

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(originalNetwork.Id, network.Id);

        wil::com_ptr<IWSLCContainer> secondContainer;
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(containerNames[1].c_str(), &secondContainer));
    }

    static void WriteComposeFile(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(content.data(), gsl::narrow<std::streamsize>(content.size()));
        stream.close();
        VERIFY_IS_TRUE(stream.good());
    }

    static wsl::windows::common::wslc_schema::InspectContainer InspectContainer(IWSLCContainer* container)
    {
        wil::unique_cotaskmem_ansistring inspectJson;
        THROW_IF_FAILED(container->Inspect(FALSE, &inspectJson));
        return wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(inspectJson.get());
    }
};
