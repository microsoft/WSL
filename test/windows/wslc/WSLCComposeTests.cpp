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
            composeFile << "services:\n"
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

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        VERIFY_IS_NOT_NULL(composeSession.get());

        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
        VERIFY_SUCCEEDED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(2u, containers.size());
        VERIFY_ARE_EQUAL(std::string("wslc-compose-basic"), std::string(containers[0].Name));
        VERIFY_ARE_EQUAL(WslcContainerStateCreated, containers[0].State);

        wil::com_ptr<IWSLCContainer> basicContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-compose-basic", &basicContainer));
        const auto inspect = InspectContainer(basicContainer.get());
        VERIFY_ARE_EQUAL(projectPath.filename().string(), inspect.Config.Labels.at("com.docker.compose.project"));
        VERIFY_ARE_EQUAL(std::string{"basic"}, inspect.Config.Labels.at("com.docker.compose.service"));
        VERIFY_ARE_EQUAL(std::string{"1"}, inspect.Config.Labels.at("com.docker.compose.container-number"));
        VERIFY_ARE_EQUAL(std::string{"False"}, inspect.Config.Labels.at("com.docker.compose.oneoff"));
        VERIFY_ARE_EQUAL(std::string{"true"}, inspect.Config.Labels.at("com.microsoft.wslc.compose.managed"));
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

        const std::string basicContainerId = containers[0].Id;
        const std::string secondaryContainerId = containers[1].Id;
        wil::com_ptr<IWSLCContainer> secondaryContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-compose-secondary", &secondaryContainer));
        VERIFY_SUCCEEDED(basicContainer->Delete(WSLCDeleteFlagsForce));
        VERIFY_SUCCEEDED(secondaryContainer->Delete(WSLCDeleteFlagsForce));

        VERIFY_SUCCEEDED(composeSession->Start());
        containers.reset();
        VERIFY_SUCCEEDED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, containers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateRunning, containers[1].State);
        VERIFY_ARE_NOT_EQUAL(basicContainerId, std::string(containers[0].Id));
        VERIFY_ARE_NOT_EQUAL(secondaryContainerId, std::string(containers[1].Id));

        VERIFY_SUCCEEDED(composeSession->Attach());
        VERIFY_SUCCEEDED(composeSession->Stop(10));
        containers.reset();
        VERIFY_SUCCEEDED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(WslcContainerStateExited, containers[0].State);
        VERIFY_ARE_EQUAL(WslcContainerStateExited, containers[1].State);
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
        std::ofstream(composePath) << "services:\n  " << std::string(WSLC_MAX_CONTAINER_NAME_LENGTH, 'a') << ":\n    image: alpine:latest\n";

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        VERIFY_IS_NULL(composeSession.get());
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->DeleteNetwork((projectPath.filename().string() + "_default").c_str()));
    }

    TEST_METHOD(ComposeEmptyServiceNameValidatedBeforeMutation)
    {
        VerifyEmptyServiceNameValidatedBeforeMutation(false);
    }

    TEST_METHOD(ComposeEmptyServiceNameWithExplicitContainerNameValidatedBeforeMutation)
    {
        VerifyEmptyServiceNameValidatedBeforeMutation(true);
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

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        auto stopCompose = wil::scope_exit([&] { LOG_IF_FAILED(composeSession->Stop(0)); });

        VERIFY_SUCCEEDED(composeSession->Start());

        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
        VERIFY_SUCCEEDED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(2u, containers.size());
        if (defaultNames)
        {
            const auto projectKey = projectPath.filename().string();
            const std::vector<std::string> expectedNames{std::format("{}-client-1", projectKey), std::format("{}-server-1", projectKey)};
            std::vector<std::string> actualNames{containers[0].Name, containers[1].Name};
            std::ranges::sort(actualNames);
            VERIFY_ARE_EQUAL(expectedNames, actualNames);
        }

        wil::com_ptr<IWSLCContainer> clientContainer;
        const auto clientName =
            defaultNames ? std::format("{}-client-1", projectPath.filename().string()) : std::string{"wslc-compose-http-client"};
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

private:
    void VerifyEmptyServiceNameValidatedBeforeMutation(bool explicitName)
    {
        const auto projectPath =
            std::filesystem::current_path() / std::format("compose-empty-service-{}-{}", explicitName, GetCurrentProcessId());
        const auto composePath = projectPath / "compose.yaml";
        const auto networkName = projectPath.filename().string() + "_default";

        VERIFY_IS_TRUE(std::filesystem::create_directory(projectPath));
        auto directoryCleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(projectPath, error);
            if (error)
            {
                LogError("Failed to cleanup project path %ws: %hs", projectPath.c_str(), error.message().c_str());
            }
        });

        {
            std::ofstream composeFile(composePath);
            composeFile << "services:\n"
                           "  \"\":\n"
                           "    image: alpine:latest\n";
            if (explicitName)
            {
                composeFile << "    container_name: " << projectPath.filename().string() << "-container\n";
            }

            composeFile.close();
            VERIFY_IS_TRUE(composeFile.good());
        }

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

    static wsl::windows::common::wslc_schema::InspectContainer InspectContainer(IWSLCContainer* container)
    {
        wil::unique_cotaskmem_ansistring inspectJson;
        THROW_IF_FAILED(container->Inspect(FALSE, &inspectJson));
        return wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(inspectJson.get());
    }
};
