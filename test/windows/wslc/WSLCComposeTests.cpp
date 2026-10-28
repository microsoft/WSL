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
