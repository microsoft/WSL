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

using wsl::test::CreateSession;
using wsl::test::GetDefaultWSLCSessionSettings;
using wsl::test::LoadTestImages;

extern bool g_fastTestRun;

namespace {

struct ComposeRequestStorage
{
    ComposeRequestStorage(const std::filesystem::path& Path, WSLCComposeAction Action)
    {
        sourcePath = std::filesystem::absolute(Path).lexically_normal();
        baseDirectory = sourcePath.parent_path();
        projectDirectory = baseDirectory;
        workingDirectory = std::filesystem::current_path();

        std::ifstream stream{sourcePath, std::ios::binary};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream);
        content.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});

        document.SourcePath = sourcePath.c_str();
        document.BaseDirectory = baseDirectory.c_str();
        document.Content = reinterpret_cast<const byte*>(content.data());
        document.ContentSize = static_cast<ULONG>(content.size());

        documents.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        documents.WorkingDirectory = workingDirectory.c_str();
        documents.ProjectDirectory = projectDirectory.c_str();
        documents.Documents = &document;
        documents.DocumentCount = 1;

        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        request.Action = Action;
        request.Project.Type = WSLCComposeProjectTypeDocuments;
        request.Project.Value.Documents = &documents;
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
    }

    ComposeRequestStorage(std::string ProjectKey, WSLCComposeAction Action) : projectKey(std::move(ProjectKey))
    {
        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        request.Action = Action;
        request.Project.Type = WSLCComposeProjectTypeProjectKey;
        request.Project.Value.ProjectKey = projectKey.c_str();
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
    }

    void SetStopTimeout(ULONG Timeout)
    {
        request.ActionOptions.Type = WSLCComposeActionOptionsTypeStop;
        request.ActionOptions.Value.Stop.TimeoutSeconds = Timeout;
    }

    std::filesystem::path sourcePath;
    std::filesystem::path baseDirectory;
    std::filesystem::path projectDirectory;
    std::filesystem::path workingDirectory;
    std::vector<char> content;
    std::string projectKey;
    WSLCComposeDocument document{};
    WSLCComposeDocuments documents{};
    WSLCComposeOperationRequest request{};
};

struct ComposeResult
{
    NON_COPYABLE(ComposeResult);

    ComposeResult() = default;

    ComposeResult(ComposeResult&& Other) noexcept : value(Other.value)
    {
        Other.value.AffectedContainers = nullptr;
        Other.value.AffectedContainersCount = 0;
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
    explicit TestComposeProgressCallback(bool BlockExecuting = false) : m_blockExecuting(BlockExecuting)
    {
        m_executingEvent.create(wil::EventOptions::ManualReset);
        m_continueEvent.create(wil::EventOptions::ManualReset);
    }

    HRESULT OnProgress(const WSLCComposeProgressEvent* Event) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, Event);
        RETURN_HR_IF(E_INVALIDARG, Event->Kind != WSLCComposeProgressEventKindStatus);

        {
            std::lock_guard lock(m_lock);
            m_orderValid = m_orderValid && Event->SequenceNumber == m_statuses.size() + 1;
            m_statuses.emplace_back(Event->Value.Status.Status);
        }

        if (m_blockExecuting && Event->Value.Status.Status == WSLCComposeStatusExecuting)
        {
            m_executingEvent.SetEvent();
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

    bool OrderValid() const
    {
        std::lock_guard lock(m_lock);
        return m_orderValid;
    }

    void WaitUntilExecuting()
    {
        const DWORD result = WaitForSingleObject(m_executingEvent.get(), 30'000);
        THROW_LAST_ERROR_IF(result == WAIT_FAILED);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), result == WAIT_TIMEOUT);
        THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0);
    }

    void Continue()
    {
        m_continueEvent.SetEvent();
    }

private:
    const bool m_blockExecuting;
    mutable std::mutex m_lock;
    std::vector<WSLCComposeStatus> m_statuses;
    bool m_orderValid{true};
    wil::unique_event m_executingEvent;
    wil::unique_event m_continueEvent;
};

ComposeResult WaitForCompose(IWSLCSession& Session, ComposeRequestStorage& Request, IComposeProgressCallback* ProgressCallback = nullptr)
{
    wil::com_ptr<IComposeOperation> operation;
    THROW_IF_FAILED(Session.BeginComposeOperation(&Request.request, ProgressCallback, &operation));

    wil::unique_handle completionEvent;
    THROW_IF_FAILED(operation->GetCompletionEvent(&completionEvent));
    THROW_LAST_ERROR_IF(WaitForSingleObject(completionEvent.get(), INFINITE) == WAIT_FAILED);

    ComposeResult result;
    THROW_IF_FAILED(operation->GetResult(&result.value));
    return result;
}

ComposeResult ExecuteCompose(IWSLCSession& Session, ComposeRequestStorage& Request, IComposeProgressCallback* ProgressCallback = nullptr)
{
    auto result = WaitForCompose(Session, Request, ProgressCallback);
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
        auto progress = Microsoft::WRL::Make<TestComposeProgressCallback>();
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
        const auto startResult = ExecuteCompose(*m_defaultSession, startRequest);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, startResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, startResult.value.AffectedContainers[1].State);
        VERIFY_ARE_EQUAL(basicContainerId, std::string(startResult.value.AffectedContainers[0].Id));
        VERIFY_ARE_EQUAL(secondaryContainerId, std::string(startResult.value.AffectedContainers[1].Id));

        ComposeRequestStorage upRequest{composePath, WSLCComposeActionUp};
        const auto upResult = ExecuteCompose(*m_defaultSession, upRequest);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, upResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, upResult.value.AffectedContainers[1].State);
        VERIFY_ARE_NOT_EQUAL(basicContainerId, std::string(upResult.value.AffectedContainers[0].Id));
        VERIFY_ARE_NOT_EQUAL(secondaryContainerId, std::string(upResult.value.AffectedContainers[1].Id));

        ComposeRequestStorage attachRequest{composePath, WSLCComposeActionAttach};
        const auto attachResult = ExecuteCompose(*m_defaultSession, attachRequest);
        VERIFY_ARE_EQUAL(2u, attachResult.value.AffectedContainersCount);

        ComposeRequestStorage stopRequest{std::string{upResult.value.ProjectKey}, WSLCComposeActionStop};
        stopRequest.SetStopTimeout(10);
        const auto stopResult = ExecuteCompose(*m_defaultSession, stopRequest);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, stopResult.value.AffectedContainers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, stopResult.value.AffectedContainers[1].State);
    }

    TEST_METHOD(ComposeContainerNameHttpRequest)
    {
        const auto projectPath = std::filesystem::current_path() / std::format("compose-http-{}", GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
        });

        std::filesystem::create_directory(projectPath);
        {
            std::ofstream composeFile(composePath);
            composeFile
                << "services:\n"
                   "  server:\n"
                   "    name: wslc-compose-http-server\n"
                   "    image: python:3.12-alpine\n"
                   "    command: [\"python3\", \"-m\", \"http.server\", \"8000\"]\n"
                   "  client:\n"
                   "    name: wslc-compose-http-client\n"
                   "    image: python:3.12-alpine\n"
                   "    command: [\"python3\", \"-c\", \"import time, urllib.request\\nwhile True:\\n    try:\\n        with "
                   "urllib.request.urlopen(\\\"http://wslc-compose-http-server:8000/\\\", timeout=10) as response:\\n            "
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

        wil::com_ptr<IWSLCContainer> clientContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-compose-http-client", &clientContainer));
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
        std::ofstream(composePath) << "services:\n"
                                      "  cancelled:\n"
                                      "    image: alpine:latest\n"
                                      "    command: [\"/bin/sh\", \"-c\", \"while true; do sleep 1; done\"]\n";

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
        VERIFY_ARE_EQUAL(0u, result.value.AffectedContainersCount);
        VERIFY_IS_TRUE(progress->OrderValid());
        VERIFY_ARE_EQUAL(WSLCComposeStatusCancelled, progress->Statuses().back());
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
            "services:\n"
            "  invalid:\n"
            "    name: {}\n"
            "    image: alpine:latest\n",
            containerName);
        const std::array unsupportedDocuments{
            std::format("include:\n  - other.yaml\n{}", service),
            std::format(
                "services:\n"
                "  invalid:\n"
                "    name: {}\n"
                "    image: alpine:latest\n"
                "    env_file: .env\n",
                containerName),
            std::format(
                "services:\n"
                "  invalid:\n"
                "    name: {}\n"
                "    image: alpine:latest\n"
                "    label_file: labels.txt\n",
                containerName),
            std::format(
                "services:\n"
                "  invalid:\n"
                "    name: {}\n"
                "    image: alpine:latest\n"
                "    extends:\n"
                "      file: common.yaml\n"
                "      service: common\n",
                containerName),
            std::format(
                "configs:\n"
                "  settings:\n"
                "    file: settings.conf\n"
                "{}",
                service),
            std::format(
                "secrets:\n"
                "  token:\n"
                "    file: token.txt\n"
                "{}",
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
        std::ofstream(composePath) << "services:\n"
                                      "  selected:\n"
                                      "    image: alpine:latest\n";

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
    static wsl::windows::common::wslc_schema::InspectContainer InspectContainer(IWSLCContainer* container)
    {
        wil::unique_cotaskmem_ansistring inspectJson;
        THROW_IF_FAILED(container->Inspect(&inspectJson));
        return wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(inspectJson.get());
    }
};
