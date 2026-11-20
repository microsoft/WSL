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

constexpr DWORD c_waitTimeoutMs = 120'000;

struct ComposeRequestStorage
{
    ComposeRequestStorage(const std::filesystem::path& path, WSLCComposeAction action)
    {
        sourcePath = std::filesystem::absolute(path).lexically_normal();
        projectDirectory = sourcePath.parent_path();

        std::ifstream stream{sourcePath, std::ios::binary};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream);
        content.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});

        document.SourcePath = sourcePath.c_str();
        document.BaseDirectory = projectDirectory.c_str();
        document.Content = reinterpret_cast<const byte*>(content.data());
        document.ContentSize = gsl::narrow<ULONG>(content.size());

        documents.ProjectDirectory = projectDirectory.c_str();
        documents.Documents = &document;
        documents.DocumentCount = 1;

        request.Action = action;
        request.Project.Type = WSLCComposeProjectTypeDocuments;
        request.Project.Value.Documents = &documents;
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
    }

    ComposeRequestStorage(std::string projectKeyValue, WSLCComposeAction action) : projectKey(std::move(projectKeyValue))
    {
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
    std::filesystem::path projectDirectory;
    std::vector<char> content;
    std::string projectKey;
    std::string explicitProjectName;
    WSLCComposeDocument document{};
    WSLCComposeDocumentCollection documents{};
    WSLCComposeOperationRequest request{};
};

struct ComposeResult
{
    NON_COPYABLE(ComposeResult);

    ComposeResult() = default;

    ComposeResult(ComposeResult&& other) noexcept : value(other.value)
    {
        other.value = {};
    }

    ~ComposeResult()
    {
        CoTaskMemFree(value.ErrorMessage);
        CoTaskMemFree(value.AffectedContainers);
    }

    WSLCComposeOperationResult value{};
};

class TestComposeProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IComposeProgressCallback, IFastRundown>
{
public:
    explicit TestComposeProgressCallback(
        bool blockExecuting = false,
        std::optional<std::string> blockProgress = std::nullopt,
        bool failOnTerminal = false,
        WSLCComposeStatus blockedStatus = WSLCComposeStatusExecuting) :
        m_blockExecuting(blockExecuting), m_blockProgress(std::move(blockProgress)), m_failOnTerminal(failOnTerminal), m_blockedStatus(blockedStatus)
    {
        m_blockedEvent.create(wil::EventOptions::ManualReset);
        m_continueEvent.create(wil::EventOptions::ManualReset);
    }

    HRESULT OnProgress(const WSLCComposeProgressEvent* event) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, event);

        bool block = false;
        HRESULT result{};
        {
            std::lock_guard lock(m_lock);
            result = m_result;
            m_orderValid = m_orderValid && event->SequenceNumber == ++m_eventCount;
            m_statuses.emplace_back(event->Status);
            if (event->Progress != nullptr)
            {
                m_progress.emplace_back(
                    event->Status,
                    event->Progress->Operation,
                    event->Progress->ResourceKey,
                    event->Progress->Current,
                    event->Progress->Total,
                    event->Progress->Unit);
                block = m_blockProgress.has_value() && Describe(m_progress.back()) == *m_blockProgress;
            }
            else
            {
                block = m_blockExecuting && event->Status == m_blockedStatus;
            }

            RETURN_HR_IF(
                RPC_E_DISCONNECTED,
                m_failOnTerminal && (event->Status == WSLCComposeStatusSucceeded || event->Status == WSLCComposeStatusFailed ||
                                     event->Status == WSLCComposeStatusCancelled));
        }

        if (block)
        {
            m_blockedEvent.SetEvent();
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !m_continueEvent.wait(c_waitTimeoutMs));
        }

        return result;
    }
    CATCH_RETURN();

    void SetResult(HRESULT result)
    {
        std::lock_guard lock(m_lock);
        m_result = result;
    }

    std::vector<WSLCComposeStatus> Statuses() const
    {
        std::lock_guard lock(m_lock);
        return m_statuses;
    }

    struct Progress
    {
        WSLCComposeStatus Status;
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
        const DWORD result = WaitForSingleObject(m_blockedEvent.get(), c_waitTimeoutMs);
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
    const bool m_failOnTerminal;
    const WSLCComposeStatus m_blockedStatus;
    mutable std::mutex m_lock;
    ULONGLONG m_eventCount{};
    std::vector<WSLCComposeStatus> m_statuses;
    std::vector<Progress> m_progress;
    bool m_orderValid{true};
    HRESULT m_result{S_OK};
    wil::unique_event m_blockedEvent;
    wil::unique_event m_continueEvent;
};

class ComposeCall
{
public:
    NON_COPYABLE(ComposeCall);
    NON_MOVABLE(ComposeCall);

    ComposeCall(IWSLCSession& session, const ComposeRequestStorage& request, TestComposeProgressCallback* progress = nullptr) :
        m_progress(progress)
    {
        // The fixture and call thread share the MTA. Request storage outlives the joined call.
        wil::com_ptr<IWSLCSession> sessionReference{&session};
        m_thread = std::thread([this, sessionReference = std::move(sessionReference), &request]() {
            m_callResult = wil::ResultFromException([&]() {
                const auto coInitialize = wil::CoInitializeEx(COINIT_MULTITHREADED);
                THROW_IF_FAILED(sessionReference->ExecuteComposeOperation(
                    &request.request, m_progress.Get(), m_cancelEvent.get(), &m_result.value));
            });
            m_completed.SetEvent();
        });
    }

    ~ComposeCall()
    {
        Cancel();
        if (m_progress)
        {
            m_progress->Continue();
        }
        m_thread.join();
    }

    void Cancel()
    {
        m_cancelEvent.SetEvent();
    }

    HANDLE CompletionEvent() const
    {
        return m_completed.get();
    }

    HRESULT WaitForReturn(DWORD timeout = c_waitTimeoutMs)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !m_completed.wait(timeout));
        return m_callResult;
    }

    ComposeResult TakeResult(DWORD timeout = c_waitTimeoutMs)
    {
        THROW_IF_FAILED(WaitForReturn(timeout));
        return std::move(m_result);
    }

private:
    Microsoft::WRL::ComPtr<TestComposeProgressCallback> m_progress;
    wil::unique_event m_cancelEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_completed{wil::EventOptions::ManualReset};
    ComposeResult m_result;
    HRESULT m_callResult{E_PENDING};
    std::thread m_thread;
};

ComposeResult WaitForCompose(IWSLCSession& session, ComposeRequestStorage& request, TestComposeProgressCallback* progressCallback = nullptr, DWORD timeout = c_waitTimeoutMs)
{
    ComposeCall call(session, request, progressCallback);
    return call.TakeResult(timeout);
}

ComposeResult ExecuteCompose(IWSLCSession& session, ComposeRequestStorage& request, TestComposeProgressCallback* progressCallback = nullptr, DWORD timeout = c_waitTimeoutMs)
{
    auto result = WaitForCompose(session, request, progressCallback, timeout);
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
        const auto projectPath = std::filesystem::current_path() / "compose-basic";
        const auto composePath = projectPath / "compose.yaml";
        const auto volumePath = projectPath / "volume";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directories(volumePath);
        WriteFileContent(volumePath / "content.txt", "compose-volume");
        WriteFileContent(
            composePath,
            "version: \"3.8\"\n"
            "services:\n"
            "  basic:\n"
            "    name: wslc-compose-basic\n"
            "    image: alpine:latest\n"
            "    environment:\n"
            "      COMPOSE_TEST: enabled\n"
            "    working_dir: /work\n"
            "    command: [\"/bin/sh\", \"-c\", \"while true; do echo $COMPOSE_TEST; sleep 1; done\"]\n"
            "    volumes:\n"
            "      - ./volume:/work:ro\n"
            "    ports:\n"
            "      - \"0:8080\"\n"
            "  secondary:\n"
            "    name: wslc-compose-secondary\n"
            "    image: alpine:latest\n"
            "    command: [\"/bin/sh\", \"-c\", \"while true; do echo secondary; sleep 1; done\"]\n");

        ComposeRequestStorage validateRequest{composePath, WSLCComposeActionValidate};
        auto validateProgress = Microsoft::WRL::Make<TestComposeProgressCallback>();
        VERIFY_IS_NOT_NULL(validateProgress.Get());

        ExecuteCompose(*m_defaultSession, validateRequest, validateProgress.Get());

        VERIFY_IS_TRUE(validateProgress->OrderValid());
        VERIFY_ARE_EQUAL(
            std::vector<WSLCComposeStatus>({WSLCComposeStatusValidating, WSLCComposeStatusSucceeded}), validateProgress->Statuses());
        VERIFY_IS_TRUE(validateProgress->ProgressEvents().empty());

        ComposeRequestStorage createRequest{composePath, WSLCComposeActionCreate};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::nullopt, true);
        VERIFY_IS_NOT_NULL(progress.Get());
        progress->SetResult(E_FAIL);

        const auto createResult = ExecuteCompose(*m_defaultSession, createRequest, progress.Get());

        VERIFY_IS_NULL(createResult.value.ErrorMessage);
        VERIFY_ARE_EQUAL(2u, createResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(std::string("wslc-compose-basic"), std::string(createResult.value.AffectedContainers[0].Name));
        VERIFY_ARE_EQUAL(WslcContainerStateCreated, createResult.value.AffectedContainers[0].State);

        VERIFY_IS_TRUE(progress->OrderValid());
        VERIFY_ARE_EQUAL(
            std::vector<WSLCComposeStatus>(
                {WSLCComposeStatusValidating, WSLCComposeStatusExecuting, WSLCComposeStatusExecuting, WSLCComposeStatusExecuting, WSLCComposeStatusExecuting, WSLCComposeStatusSucceeded}),
            progress->Statuses());

        VERIFY_ARE_EQUAL(
            std::vector<std::string>(
                {std::format("network:create:{}_default:1/1", projectPath.filename().string()),
                 "container:create:wslc-compose-basic:1/2",
                 "container:create:wslc-compose-secondary:2/2"}),
            progress->ProgressDescriptions());

        for (const auto& resourceProgress : progress->ProgressEvents())
        {
            VERIFY_ARE_EQUAL(WSLCComposeStatusExecuting, resourceProgress.Status);
        }

        wil::com_ptr<IWSLCContainer> basicContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-compose-basic", &basicContainer));
        const auto inspect = InspectContainer(basicContainer.get());

        VERIFY_IS_FALSE(inspect.Config.Labels.contains("com.microsoft.wslc.compose.managed"));
        VERIFY_IS_FALSE(inspect.Config.Labels.contains("com.microsoft.wslc.compose.oneoff"));
        VERIFY_ARE_EQUAL(std::string{"1"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.metadata-version"));

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

        VERIFY_ARE_EQUAL(projectPath.filename().string(), inspect.Config.Labels.at("com.microsoft.wslc.compose.project"));
        VERIFY_ARE_EQUAL(std::string{"basic"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.service"));
        VERIFY_ARE_EQUAL(std::string{"1"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.container-number"));

        wil::unique_cotaskmem_ansistring networkOutput;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(std::format("{}_default", projectPath.filename().string()).c_str(), &networkOutput));
        const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(networkOutput.get());

        VERIFY_ARE_EQUAL(projectPath.filename().string(), network.Labels.at("com.microsoft.wslc.compose.project"));
        VERIFY_ARE_EQUAL(std::string{"default"}, network.Labels.at("com.microsoft.wslc.compose.network"));
        VERIFY_IS_FALSE(network.Labels.contains("com.microsoft.wslc.compose.managed"));

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
        auto upProgress = Microsoft::WRL::Make<TestComposeProgressCallback>(false, "container:remove:wslc-compose-basic:1/2");
        VERIFY_IS_NOT_NULL(upProgress.Get());

        ComposeCall upCall(*m_defaultSession, upRequest, upProgress.Get());
        upProgress->WaitUntilBlocked();

        VERIFY_SUCCEEDED(basicContainer->Delete(WSLCDeleteFlagsForce));
        upProgress->Continue();

        const auto upResult = upCall.TakeResult();
        VERIFY_SUCCEEDED(upResult.value.Result);
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

    TEST_METHOD(ComposeNetworkCollisionRejectedBeforeMutation)
    {
        const std::string projectName{"compose-network-collision"};
        const auto projectPath = std::filesystem::current_path() / projectName;
        const auto composePath = projectPath / "compose.yaml";
        const auto networkName = projectName + "_default";
        const auto containerName = projectName + "-service-1";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  service:\n    image: alpine:latest\n");

        const std::vector<std::map<std::string, std::string>> labelSets{
            {},
            {{"com.docker.compose.project", projectName}, {"com.docker.compose.network", "default"}},
            {{"com.microsoft.wslc.compose.project", "another-project"},
             {"com.microsoft.wslc.compose.network", "default"},
             {"com.microsoft.wslc.compose.metadata-version", "1"}},
            {{"com.microsoft.wslc.compose.project", projectName},
             {"com.microsoft.wslc.compose.network", "another-network"},
             {"com.microsoft.wslc.compose.metadata-version", "1"}},
            {{"com.microsoft.wslc.compose.project", projectName}, {"com.microsoft.wslc.compose.network", "default"}},
            {{"com.microsoft.wslc.compose.project", projectName},
             {"com.microsoft.wslc.compose.network", "default"},
             {"com.microsoft.wslc.compose.metadata-version", "2"}}};

        for (const auto& labelSet : labelSets)
        {
            std::vector<WSLCLabel> labels;
            for (const auto& [key, value] : labelSet)
            {
                labels.push_back({key.c_str(), value.c_str()});
            }

            WSLCNetworkOptions options{};
            options.Name = networkName.c_str();
            options.Labels = labels.data();
            options.LabelsCount = gsl::narrow<ULONG>(labels.size());
            VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));
            auto networkCleanup = wil::scope_exit([&] { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            wil::unique_cotaskmem_ansistring originalOutput;
            VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &originalOutput));
            const auto originalNetwork = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(originalOutput.get());
            VERIFY_IS_FALSE(originalNetwork.Id.empty());

            ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
            const auto result = WaitForCompose(*m_defaultSession, request);

            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), result.value.Result);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
            VERIFY_IS_NOT_NULL(result.value.ErrorMessage);
            VERIFY_ARE_EQUAL(wsl::shared::Localization::MessageWslcComposeNetworkConflict(networkName), std::wstring{result.value.ErrorMessage});

            wil::unique_cotaskmem_ansistring output;
            VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
            const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
            VERIFY_ARE_EQUAL(originalNetwork.Id, network.Id);
            VERIFY_IS_TRUE(originalNetwork.Labels == network.Labels);

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(containerName.c_str(), &container));
        }
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

    TEST_METHOD(ComposeInvalidServiceCharactersValidatedBeforeMutation)
    {
        for (const std::string_view serviceName :
             {"service/name", "service name", "service:name", "service@name", "service\xc3\xa9"})
        {
            VerifyInvalidServiceNameValidatedBeforeMutation(serviceName, true);
        }
    }

    TEST_METHOD(ComposeServiceNameAllowsSpecCharacters)
    {
        const std::string projectName{"compose-service-characters"};
        const auto projectPath = std::filesystem::current_path() / projectName;
        const auto composePath = projectPath / "compose.yaml";
        const auto containerName = projectName + "-Web_09.api-v1-1";
        const auto networkName = projectName + "_default";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  Web_09.api-v1:\n    image: alpine:latest\n");

        wil::com_ptr<IWSLCContainer> container;
        auto resourceCleanup = wil::scope_exit([&] {
            if (container)
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        });

        ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
        const auto result = ExecuteCompose(*m_defaultSession, request);

        VERIFY_ARE_EQUAL(1u, result.value.AffectedContainersCount);
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container));
        const auto inspect = InspectContainer(container.get());
        VERIFY_ARE_EQUAL(std::string{"Web_09.api-v1"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.service"));
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
        const std::string projectName{"compose-resolved-names"};
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
        const std::string projectName{"compose-explicit-names"};
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
        const std::string projectName{"compose-nul-names"};
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

    TEST_METHOD(ComposeProjectIdentitySharedAcrossFiles)
    {
        VerifyProjectIdentitySharedAcrossFiles(false);
    }

    TEST_METHOD(ComposeProjectIdentitySharedAcrossNormalizedDirectories)
    {
        VerifyProjectIdentitySharedAcrossFiles(true);
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
        const auto projectPath = std::filesystem::current_path() / std::format("compose-http-{}", defaultNames);
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        WriteFileContent(
            composePath,
            std::format(
                "services:\n"
                "  server:\n"
                "{}"
                "    image: python:3.12-alpine\n"
                "    command: [\"python3\", \"-m\", \"http.server\", \"8000\"]\n"
                "  client:\n"
                "{}"
                "    image: python:3.12-alpine\n"
                "    command: [\"python3\", \"-c\", \"import time, urllib.request\\nwhile True:\\n    try:\\n        with "
                "urllib.request.urlopen(\\\"http://{}:8000/\\\", timeout=10) as response:\\n            "
                "if response.status == 200:\\n                break\\n    except Exception:\\n        pass\\n    "
                "time.sleep(1)\"]\n",
                defaultNames ? "" : "    name: wslc-compose-http-server\n",
                defaultNames ? "" : "    name: wslc-compose-http-client\n",
                defaultNames ? "server" : "wslc-compose-http-server"));

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
            const std::vector<std::string> actualNames{
                upResult.value.AffectedContainers[0].Name, upResult.value.AffectedContainers[1].Name};
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
        const auto projectPath = std::filesystem::current_path() / "compose-cancel";
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        WriteFileContent(composePath, R"(
services:
  cancelled:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)");

        ComposeRequestStorage request{composePath, WSLCComposeActionUp};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall operation(*m_defaultSession, request, progress.Get());
        auto ensureUnblocked = wil::scope_exit([&] {
            operation.Cancel();
            progress->Continue();
        });

        progress->WaitUntilExecuting();
        operation.Cancel();
        progress->Continue();

        const auto result = operation.TakeResult();
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(projectPath.filename().string(), std::string{result.value.ProjectKey});
        VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        VERIFY_IS_TRUE(progress->OrderValid());
        VERIFY_ARE_EQUAL(WSLCComposeStatusCancelled, progress->Statuses().back());
    }

    TEST_METHOD(ComposeLateCancellationDoesNotOverrideCompletedMutation)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-late-cancel";
        const auto composePath = projectPath / "compose.yaml";
        const auto markerPath = projectPath / "term-received";
        const auto readyPath = projectPath / "ready";
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

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        WriteFileContent(composePath, R"(
services:
  late-cancel:
    image: alpine:latest
    command: ["/bin/sh", "-c", "trap 'touch /work/term-received; sleep 30' TERM; touch /work/ready; while true; do sleep 1; done"]
    volumes:
      - ./:/work
)");

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(readyPath)); },
            100ms,
            std::chrono::milliseconds{c_waitTimeoutMs});

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(5);
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall operation(*m_defaultSession, stopRequest, progress.Get());
        auto ensureUnblocked = wil::scope_exit([&] {
            progress->Continue();
            operation.Cancel();
        });

        progress->WaitUntilExecuting();
        progress->Continue();

        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(markerPath)); },
            100ms,
            std::chrono::milliseconds{c_waitTimeoutMs});
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(operation.CompletionEvent(), 0));
        operation.Cancel();

        const auto result = operation.TakeResult();
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusSucceeded, result.value.Status);
        VERIFY_SUCCEEDED(result.value.Result);
        VERIFY_ARE_EQUAL(std::string{upResult.value.ProjectKey}, std::string{result.value.ProjectKey});
        VERIFY_ARE_EQUAL(1u, result.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, result.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WSLCComposeStatusSucceeded, progress->Statuses().back());
    }

    TEST_METHOD(ComposeCancellationAfterMutationLeavesDiscoverablePartialState)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-partial-cancel";
        const auto composePath = projectPath / "compose.yaml";
        const auto markerPath = projectPath / "term-received";
        const auto readyPath = projectPath / "ready";
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

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        WriteFileContent(composePath, R"(
services:
  first:
    image: alpine:latest
    command: ["/bin/sh", "-c", "trap 'touch /work/term-received; sleep 30' TERM; touch /work/ready; while true; do sleep 1; done"]
    volumes:
      - ./:/work
  second:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)");

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(readyPath)); },
            100ms,
            std::chrono::milliseconds{c_waitTimeoutMs});

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(5);
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall operation(*m_defaultSession, stopRequest, progress.Get());
        auto ensureUnblocked = wil::scope_exit([&] {
            progress->Continue();
            operation.Cancel();
        });

        progress->WaitUntilExecuting();
        progress->Continue();

        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), !std::filesystem::exists(markerPath)); },
            100ms,
            std::chrono::milliseconds{c_waitTimeoutMs});
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(operation.CompletionEvent(), 0));
        operation.Cancel();

        const auto result = operation.TakeResult();
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
        const auto projectPath = std::filesystem::current_path() / "compose-create-cancel";
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
        WriteFileContent(composePath, R"(
services:
  first:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
  second:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)");

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        auto progress =
            Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::format("container:create:{}:2/2", secondContainerName));
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall operation(*m_defaultSession, upRequest, progress.Get());
        auto ensureUnblocked = wil::scope_exit([&] {
            operation.Cancel();
            progress->Continue();
        });

        progress->WaitUntilBlocked();
        operation.Cancel();
        progress->Continue();

        const auto result = operation.TakeResult();
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
        const auto projectPath = std::filesystem::current_path() / "compose-name-length";
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
            WriteFileContent(
                composePath,
                std::format(
                    R"(
services:
  {}:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)",
                    serviceName));

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
        const auto projectPath = std::filesystem::current_path() / "compose-discovery";
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
        WriteFileContent(composePath, R"(
services:
  service:
    image: alpine:latest
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)");

        WSLCContainerLauncher launcher("alpine:latest", containerName, {"/bin/sh", "-c", "while true; do sleep 1; done"});
        launcher.AddLabel("com.microsoft.wslc.compose.project", projectKey);
        launcher.AddLabel("com.microsoft.wslc.compose.service", "service");
        launcher.AddLabel("com.microsoft.wslc.compose.container-number", "1");
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

    TEST_METHOD(ComposeDiscoveryRecoversRunningInitProcess)
    {
        WSADATA wsaData{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &wsaData));
        auto winsockCleanup = wil::scope_exit([] { WSACleanup(); });

        const auto runDocker = [&](const std::vector<std::string>& arguments) {
            wsl::windows::common::WSLCProcessLauncher launcher("/usr/bin/docker", arguments);
            const auto result = launcher.Launch(*m_defaultSession).WaitAndCaptureOutput(c_waitTimeoutMs);
            VERIFY_ARE_EQUAL(0, result.Code);
            return result.Output.at(1);
        };

        for (const bool tty : {false, true})
        {
            const auto projectKey = std::format("compose-running-recovery-{}", tty);
            const auto containerName = projectKey + "-service";
            const auto flags = tty ? WSLCProcessFlagsTty : WSLCProcessFlagsNone;
            WSLCContainerLauncher templateLauncher("alpine:latest", projectKey + "-template", {"/bin/sleep", "99999"}, {}, "host", flags);
            auto templateContainer = templateLauncher.Create(*m_defaultSession);
            const auto inspect = nlohmann::json::parse(runDocker({"/usr/bin/docker", "inspect", templateContainer.Id()}));
            const auto metadata = inspect.at(0).at("Config").at("Labels").at("com.microsoft.wsl.container.metadata").get<std::string>();

            auto cleanup = wil::scope_exit([&] {
                LOG_IF_FAILED(wil::ResultFromException([&] {
                    wsl::windows::common::WSLCProcessLauncher launcher("/usr/bin/docker", {"/usr/bin/docker", "rm", "-f", containerName});
                    launcher.Launch(*m_defaultSession).WaitAndCaptureOutput(c_waitTimeoutMs);
                }));
            });

            // Docker-created containers remain uncached until Compose discovery recovers them.
            std::vector<std::string> arguments{
                "/usr/bin/docker",
                "run",
                "--detach",
                "--name",
                containerName,
                "--network",
                "host",
                "--label",
                "com.microsoft.wsl.container.metadata=" + metadata,
                "--label",
                "com.microsoft.wslc.compose.project=" + projectKey,
                "--label",
                "com.microsoft.wslc.compose.service=service",
                "--label",
                "com.microsoft.wslc.compose.container-number=1",
                "--label",
                "com.microsoft.wslc.compose.metadata-version=1",
            };
            if (tty)
            {
                arguments.emplace_back("--tty");
            }
            arguments.insert(arguments.end(), {"alpine:latest", "/bin/sleep", "99999"});
            runDocker(arguments);

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_ARE_EQUAL(E_UNEXPECTED, m_defaultSession->OpenContainer(containerName.c_str(), &container));
            ComposeRequestStorage startRequest{projectKey, WSLCComposeActionStart};
            const auto started = ExecuteCompose(*m_defaultSession, startRequest);
            VERIFY_ARE_EQUAL(1u, started.value.AffectedContainersCount);
            VERIFY_ARE_EQUAL(WslcContainerStateRunning, started.value.AffectedContainers[0].State);
            VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container));

            wil::com_ptr<IWSLCProcess> process;
            VERIFY_SUCCEEDED(container->GetInitProcess(&process));

            WSLCProcessFlags recoveredFlags{};
            VERIFY_SUCCEEDED(process->GetFlags(&recoveredFlags));
            VERIFY_ARE_EQUAL(flags, recoveredFlags);

            WSLCProcessState state{};
            int exitCode{};
            VERIFY_SUCCEEDED(process->GetState(&state, &exitCode));
            VERIFY_ARE_EQUAL(WslcProcessStateRunning, state);
            VERIFY_ARE_EQUAL(-1, exitCode);

            wsl::windows::common::wslutil::COMOutputHandle stdinHandle;
            wsl::windows::common::wslutil::COMOutputHandle stdoutHandle;
            wsl::windows::common::wslutil::COMOutputHandle stderrHandle;
            VERIFY_SUCCEEDED(container->Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle));
            if (tty)
            {
                VERIFY_SUCCEEDED(process->ResizeTty(30, 100));
                VERIFY_IS_FALSE(stdinHandle.Empty());
                VERIFY_IS_TRUE(stdoutHandle.Empty());
                VERIFY_IS_TRUE(stderrHandle.Empty());
            }
            else
            {
                VERIFY_IS_FALSE(stdoutHandle.Empty());
                VERIFY_IS_FALSE(stderrHandle.Empty());
            }

            wil::unique_handle exitEvent;
            VERIFY_SUCCEEDED(process->GetExitEvent(exitEvent.put()));
            VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(exitEvent.get(), 0));

            VERIFY_SUCCEEDED(process->Signal(WSLCSignalSIGKILL));
            VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(exitEvent.get(), c_waitTimeoutMs));

            VERIFY_SUCCEEDED(process->GetState(&state, &exitCode));
            VERIFY_ARE_EQUAL(WslcProcessStateExited, state);
            VERIFY_ARE_EQUAL(128 + WSLCSignalSIGKILL, exitCode);

            WSLCContainerState containerState{};
            VERIFY_SUCCEEDED(container->GetState(&containerState));
            VERIFY_ARE_EQUAL(WslcContainerStateExited, containerState);
        }
    }

    TEST_METHOD(ComposeStopUsesConfiguredSignalAndObservesAutoRemoval)
    {
        for (const bool autoRemove : {false, true})
        {
            const auto projectKey = std::format("compose-stop-signal-{}", autoRemove);
            const auto containerName = projectKey + "-service";
            WSLCContainerLauncher launcher("alpine:latest", containerName, {"/bin/cat"}, {}, "host", WSLCProcessFlagsStdin);
            launcher.AddLabel("com.microsoft.wslc.compose.project", projectKey);
            launcher.AddLabel("com.microsoft.wslc.compose.service", "service");
            launcher.AddLabel("com.microsoft.wslc.compose.container-number", "1");
            launcher.AddLabel("com.microsoft.wslc.compose.metadata-version", "1");
            launcher.SetDefaultStopSignal(WSLCSignalSIGHUP);
            launcher.SetContainerFlags(autoRemove ? WSLCContainerFlagsInit | WSLCContainerFlagsRm : WSLCContainerFlagsInit);
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            const auto id = container.Id();

            ComposeRequestStorage request{projectKey, WSLCComposeActionStop};
            request.SetStopTimeout(30);
            const auto result = ExecuteCompose(*m_defaultSession, request);
            VERIFY_ARE_EQUAL(WSLCSignalSIGHUP + 128, process.Wait(c_waitTimeoutMs));
            VERIFY_ARE_EQUAL(1u, result.value.AffectedContainersCount);
            VERIFY_ARE_EQUAL(id, std::string{result.value.AffectedContainers[0].Id});
            VERIFY_ARE_EQUAL(std::string{"alpine:latest"}, std::string{result.value.AffectedContainers[0].Image});
            VERIFY_ARE_EQUAL(autoRemove ? WslcContainerStateDeleted : WslcContainerStateExited, result.value.AffectedContainers[0].State);
            if (autoRemove)
            {
                container.SetDeleteOnClose(false);
                wil::com_ptr<IWSLCContainer> reopened;
                VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(id.c_str(), &reopened));
            }
        }
    }

    TEST_METHOD(ComposeDiscoveryValidatesAndSortsContainerNumbers)
    {
        const std::string projectKey{"compose-discovery-order"};
        std::vector<wsl::windows::common::RunningWSLCContainer> containers;
        std::map<uint64_t, std::string> containerIds;

        const auto createContainer = [&](std::string_view suffix, std::string_view containerNumber) {
            WSLCContainerLauncher launcher(
                "alpine:latest", std::format("{}-{}", projectKey, suffix), {"/bin/sh", "-c", "while true; do sleep 1; done"});
            launcher.AddLabel("com.microsoft.wslc.compose.project", projectKey);
            launcher.AddLabel("com.microsoft.wslc.compose.service", "service");
            launcher.AddLabel("com.microsoft.wslc.compose.container-number", std::string{containerNumber});
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
        const auto projectPath = std::filesystem::current_path() / "compose-invalid";
        const auto composePath = projectPath / "compose.yaml";
        const std::string containerName{"wslc-compose-invalid"};
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
            WriteFileContent(composePath, document);

            ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
            const auto result = WaitForCompose(*m_defaultSession, request);
            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
            VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        }

        wil::com_ptr<IWSLCContainer> container;
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, m_defaultSession->OpenContainer(containerName.c_str(), &container));
    }

    TEST_METHOD(ComposeFailureDescriptionSurvivesObserverFailure)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-error";
        const auto composePath = projectPath / "compose.yaml";
        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  bad name:\n    image: alpine:latest\n");

        const auto expectedMessage = wsl::shared::Localization::MessageWslcComposeServiceNameInvalid("bad name");
        for (const bool failingObserver : {false, true})
        {
            ComposeRequestStorage request{composePath, WSLCComposeActionValidate};
            auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::nullopt, true);
            VERIFY_IS_NOT_NULL(progress.Get());
            progress->SetResult(E_FAIL);

            const auto result = WaitForCompose(*m_defaultSession, request, failingObserver ? progress.Get() : nullptr);

            VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
            VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
            VERIFY_IS_NOT_NULL(result.value.ErrorMessage);
            VERIFY_ARE_EQUAL(expectedMessage, std::wstring{result.value.ErrorMessage});
            VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);

            if (failingObserver)
            {
                VERIFY_IS_TRUE(progress->OrderValid());
                VERIFY_ARE_EQUAL(std::vector<WSLCComposeStatus>({WSLCComposeStatusValidating, WSLCComposeStatusFailed}), progress->Statuses());
                VERIFY_IS_TRUE(progress->ProgressEvents().empty());
            }
        }
    }

    TEST_METHOD(ComposeCallReturnsOnSessionProcessExit)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-owner-exit";
        const auto composePath = projectPath / "compose.yaml";
        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  service:\n    image: alpine:latest\n");

        const auto settings = GetDefaultWSLCSessionSettings(L"wslc-compose-owner-exit");
        auto session = CreateSession(settings);

        wil::unique_handle sessionProcess;
        VERIFY_SUCCEEDED(session->GetProcessHandle(&sessionProcess));
        auto sessionCleanup = wil::scope_exit([&] {
            if (WaitForSingleObject(sessionProcess.get(), 0) == WAIT_TIMEOUT)
            {
                LOG_IF_FAILED(session->Terminate());
            }
        });

        wil::unique_handle defaultSessionProcess;
        VERIFY_SUCCEEDED(m_defaultSession->GetProcessHandle(&defaultSessionProcess));
        VERIFY_IS_FALSE(CompareObjectHandles(sessionProcess.get(), defaultSessionProcess.get()));
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(sessionProcess.get(), 0));

        ComposeRequestStorage request{composePath, WSLCComposeActionUp};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall operation(*session, request, progress.Get());
        auto ensureUnblocked = wil::scope_exit([&] { progress->Continue(); });
        progress->WaitUntilExecuting();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(operation.CompletionEvent(), 0));

        VERIFY_WIN32_BOOL_SUCCEEDED(TerminateProcess(sessionProcess.get(), ERROR_PROCESS_ABORTED));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_S_CALL_FAILED), operation.WaitForReturn());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(defaultSessionProcess.get(), 0));
    }

    TEST_METHOD(ComposeShutdownCancelsBlockedCallback)
    {
        auto session = CreateSession(GetDefaultWSLCSessionSettings(L"wslc-compose-callback-shutdown"));
        ComposeRequestStorage request{std::string{"compose-callback-shutdown"}, WSLCComposeActionAttach};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>(true, std::nullopt, false, WSLCComposeStatusValidating);
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall call(*session, request, progress.Get());
        progress->WaitUntilBlocked();

        wil::unique_event terminated{wil::EventOptions::ManualReset};
        HRESULT terminateResult{E_PENDING};
        std::thread terminateThread([&]() {
            terminateResult = wil::ResultFromException([&]() {
                const auto coInitialize = wil::CoInitializeEx(COINIT_MULTITHREADED);
                THROW_IF_FAILED(session->Terminate());
            });
            terminated.SetEvent();
        });
        auto joinTermination = wil::scope_exit([&]() {
            progress->Continue();
            terminateThread.join();
        });

        const auto result = call.TakeResult();
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_IS_TRUE(terminated.wait(c_waitTimeoutMs));
        VERIFY_SUCCEEDED(terminateResult);
    }

    TEST_METHOD(ComposePresignaledCancellationPreventsExecution)
    {
        ComposeRequestStorage request{std::string{"compose-presignaled"}, WSLCComposeActionAttach};
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>();
        VERIFY_IS_NOT_NULL(progress.Get());

        wil::unique_event cancel{wil::EventOptions::ManualReset};
        cancel.SetEvent();
        ComposeResult result;

        VERIFY_SUCCEEDED(m_defaultSession->ExecuteComposeOperation(&request.request, progress.Get(), cancel.get(), &result.value));

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), result.value.Result);
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, result.value.Status);
        VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(std::vector<WSLCComposeStatus>{WSLCComposeStatusCancelled}, progress->Statuses());
        VERIFY_IS_TRUE(progress->ProgressEvents().empty());
    }

    TEST_METHOD(ComposeSameProjectSerializesAndQueuedOperationCanCancel)
    {
        const std::string projectKey{"compose-serialized"};
        const auto projectPath = std::filesystem::current_path() / projectKey;
        const auto composePath = projectPath / "compose.yaml";
        const auto containerName = projectKey + "-service-1";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto cleanup = wil::scope_exit([&] {
            wil::com_ptr<IWSLCContainer> container;
            if (SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container)))
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork((projectKey + "_default").c_str()));
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  service:\n    image: alpine:latest\n");

        ComposeRequestStorage createRequest{composePath, WSLCComposeActionCreate};
        auto creatingProgress =
            Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::format("container:create:{}:1/1", containerName));
        VERIFY_IS_NOT_NULL(creatingProgress.Get());

        ComposeCall creating(*m_defaultSession, createRequest, creatingProgress.Get());
        auto finishCreating = wil::scope_exit([&] {
            creating.Cancel();
            creatingProgress->Continue();
        });

        creatingProgress->WaitUntilBlocked();

        ComposeRequestStorage startRequest{std::string{projectKey}, WSLCComposeActionStart};
        auto queuedProgress = Microsoft::WRL::Make<TestComposeProgressCallback>(true);
        VERIFY_IS_NOT_NULL(queuedProgress.Get());

        ComposeCall queued(*m_defaultSession, startRequest, queuedProgress.Get());
        auto finishQueued = wil::scope_exit([&] {
            queued.Cancel();
            queuedProgress->Continue();
        });

        queuedProgress->WaitUntilExecuting();
        queuedProgress->Continue();

        ComposeRequestStorage validateRequest{composePath, WSLCComposeActionValidate};
        const auto validated = ExecuteCompose(*m_defaultSession, validateRequest);
        VERIFY_ARE_EQUAL(projectKey, std::string{validated.value.ProjectKey});
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(queued.CompletionEvent(), 1'000));

        queued.Cancel();
        const auto cancelled = queued.TakeResult();
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusCancelled, cancelled.value.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_CANCELLED), cancelled.value.Result);
        VERIFY_ARE_EQUAL(0u, cancelled.value.AffectedContainersCount);
        VERIFY_IS_TRUE(queuedProgress->ProgressEvents().empty());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(creating.CompletionEvent(), 0));

        creatingProgress->Continue();
        const auto created = creating.TakeResult();
        VERIFY_SUCCEEDED(created.value.Result);
        VERIFY_ARE_EQUAL(1u, created.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(containerName, std::string{created.value.AffectedContainers[0].Name});
    }

    TEST_METHOD(ComposeDifferentProjectsProceedIndependently)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-independent";
        const auto composePath = projectPath / "compose.yaml";
        const std::array projectKeys{std::string{"compose-blocked"}, std::string{"compose-unblocked"}};

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto cleanup = wil::scope_exit([&] {
            for (const auto& projectKey : projectKeys)
            {
                wil::com_ptr<IWSLCContainer> container;
                if (SUCCEEDED(m_defaultSession->OpenContainer((projectKey + "-service-1").c_str(), &container)))
                {
                    LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
                }

                LOG_IF_FAILED(m_defaultSession->DeleteNetwork((projectKey + "_default").c_str()));
            }

            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        WriteFileContent(composePath, "services:\n  service:\n    image: alpine:latest\n");

        ComposeRequestStorage blockedRequest{composePath, WSLCComposeActionCreate};
        blockedRequest.SetProjectName(projectKeys[0]);
        auto progress =
            Microsoft::WRL::Make<TestComposeProgressCallback>(false, std::format("container:create:{}-service-1:1/1", projectKeys[0]));
        VERIFY_IS_NOT_NULL(progress.Get());

        ComposeCall blocked(*m_defaultSession, blockedRequest, progress.Get());
        auto finishBlocked = wil::scope_exit([&] {
            blocked.Cancel();
            progress->Continue();
        });

        progress->WaitUntilBlocked();

        ComposeRequestStorage independentRequest{composePath, WSLCComposeActionCreate};
        independentRequest.SetProjectName(projectKeys[1]);
        const auto independent = ExecuteCompose(*m_defaultSession, independentRequest);
        VERIFY_ARE_EQUAL(projectKeys[1], std::string{independent.value.ProjectKey});
        VERIFY_ARE_EQUAL(1u, independent.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(projectKeys[1] + "-service-1", std::string{independent.value.AffectedContainers[0].Name});
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(blocked.CompletionEvent(), 0));

        progress->Continue();
        const auto result = blocked.TakeResult();
        VERIFY_SUCCEEDED(result.value.Result);
        VERIFY_ARE_EQUAL(projectKeys[0], std::string{result.value.ProjectKey});
    }

    TEST_METHOD(ComposeInvalidDocumentEnvelopeRejectedBeforeExecution)
    {
        const auto projectPath = std::filesystem::current_path() / "compose-envelope";
        const auto composePath = projectPath / "compose.yaml";
        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });
        WriteFileContent(composePath, "services:\n  service:\n    image: alpine:latest\n");

        ComposeRequestStorage request{composePath, WSLCComposeActionValidate};
        ComposeResult rejected;
        request.document.ContentSize = 0;
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->ExecuteComposeOperation(&request.request, nullptr, nullptr, &rejected.value));
        VERIFY_IS_NULL(rejected.value.AffectedContainers);

        request.document.ContentSize = gsl::narrow<ULONG>(request.content.size());
        request.document.BaseDirectory = nullptr;
        VERIFY_ARE_EQUAL(E_POINTER, m_defaultSession->ExecuteComposeOperation(&request.request, nullptr, nullptr, &rejected.value));
        VERIFY_IS_NULL(rejected.value.AffectedContainers);

        request.document.BaseDirectory = L"";
        const auto result = WaitForCompose(*m_defaultSession, request);
        VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
    }

private:
    void VerifyInvalidServiceNameValidatedBeforeMutation(std::string_view serviceName, bool useExplicitContainerName)
    {
        const auto projectName = std::format("compose-service-name-{}", useExplicitContainerName);
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

        WriteFileContent(composePath, content);

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));
        auto networkCleanup = wil::scope_exit([&] { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        wil::unique_cotaskmem_ansistring originalOutput;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &originalOutput));
        const auto originalNetwork = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(originalOutput.get());
        VERIFY_IS_FALSE(originalNetwork.Id.empty());

        ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
        const auto result = WaitForCompose(*m_defaultSession, request);
        VERIFY_ARE_EQUAL(E_INVALIDARG, result.value.Result);
        VERIFY_ARE_EQUAL(WSLCComposeOperationStatusFailed, result.value.Status);
        VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(originalNetwork.Id, network.Id);
    }

    void VerifyUtf16ComposeDocument(bool bigEndian)
    {
        const auto projectName = std::format("compose-utf16-{}", bigEndian);
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

        WriteFileContent(composePath, content);

        ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
        const auto result = ExecuteCompose(*m_defaultSession, request);
        VERIFY_ARE_EQUAL(1u, result.value.AffectedContainersCount);
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
        VERIFY_ARE_EQUAL(std::string{"encoded"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.service"));
        VERIFY_IS_TRUE(inspect.Config.Env.has_value());
        VERIFY_IS_TRUE(std::ranges::find(*inspect.Config.Env, std::string{"ENCODING_TEST=\xc3\xa9"}) != inspect.Config.Env->end());
    }

    void VerifyProjectIdentitySharedAcrossFiles(bool differentDirectory)
    {
        const auto projectName = std::format("compose-project-files-{}", differentDirectory);
        const auto sourceRoot = std::filesystem::current_path() / (projectName + "-source");
        const auto projectPath = sourceRoot / projectName;
        const auto alternateProjectPath = differentDirectory ? sourceRoot / (projectName + "!") : projectPath;
        const auto composePath = projectPath / "compose.yaml";
        const auto alternateComposePath = alternateProjectPath / (differentDirectory ? "compose.yaml" : "alternate.yaml");
        const auto networkName = projectName + "_default";
        const auto containerName = projectName + "-first-1";

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

        constexpr std::string_view c_content = "services:\n  first:\n    image: alpine:latest\n";
        WriteFileContent(composePath, c_content);
        WriteFileContent(alternateComposePath, c_content);

        ComposeRequestStorage request{composePath, WSLCComposeActionCreate};
        const auto createResult = ExecuteCompose(*m_defaultSession, request);
        wil::com_ptr<IWSLCContainer> container;
        auto resourceCleanup = wil::scope_exit([&] {
            if (container)
            {
                LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce));
            }

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        });

        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &container));
        VERIFY_ARE_EQUAL(projectName, std::string{createResult.value.ProjectKey});
        VERIFY_ARE_EQUAL(1u, createResult.value.AffectedContainersCount);
        const std::string originalContainerId = createResult.value.AffectedContainers[0].Id;
        VERIFY_IS_FALSE(originalContainerId.empty());

        wil::unique_cotaskmem_ansistring originalOutput;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &originalOutput));
        const auto originalNetwork = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(originalOutput.get());
        VERIFY_IS_FALSE(originalNetwork.Id.empty());

        const auto reusedResult = ExecuteCompose(*m_defaultSession, request);
        VERIFY_ARE_EQUAL(1u, reusedResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(originalContainerId, std::string{reusedResult.value.AffectedContainers[0].Id});

        ComposeRequestStorage alternateRequest{alternateComposePath, WSLCComposeActionCreate};
        const auto alternateResult = ExecuteCompose(*m_defaultSession, alternateRequest);
        VERIFY_ARE_EQUAL(projectName, std::string{alternateResult.value.ProjectKey});
        VERIFY_ARE_EQUAL(1u, alternateResult.value.AffectedContainersCount);
        VERIFY_ARE_EQUAL(originalContainerId, std::string{alternateResult.value.AffectedContainers[0].Id});
        VERIFY_ARE_EQUAL(containerName, std::string{alternateResult.value.AffectedContainers[0].Name});

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        const auto network = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(originalNetwork.Id, network.Id);
    }

    static void WriteFileContent(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream stream(path, std::ios::binary);
        VERIFY_IS_TRUE(stream.is_open());

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
