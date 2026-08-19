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
        const auto composePath = std::filesystem::current_path() / std::format("compose-{}.yaml", GetCurrentProcessId());
        const auto volumePath = std::filesystem::current_path() / std::format("compose-volume-{}", GetCurrentProcessId());
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove(composePath, error);
            std::filesystem::remove_all(volumePath, error);
        });

        std::filesystem::create_directory(volumePath);
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

    TEST_METHOD(ComposeContainerNameHttpRequest)
    {
        const auto composePath = std::filesystem::current_path() / std::format("compose-http-{}.yaml", GetCurrentProcessId());
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove(composePath, error);
        });

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

        wil::com_ptr<IWSLCComposeSession> composeSession;
        VERIFY_SUCCEEDED(m_defaultSession->CreateComposeSession(composePath.c_str(), &composeSession));
        auto stopCompose = wil::scope_exit([&] { LOG_IF_FAILED(composeSession->Stop(0)); });

        VERIFY_SUCCEEDED(composeSession->Start());

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

private:
    static wsl::windows::common::wslc_schema::InspectContainer InspectContainer(IWSLCContainer* container)
    {
        wil::unique_cotaskmem_ansistring inspectJson;
        THROW_IF_FAILED(container->Inspect(&inspectJson));
        return wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(inspectJson.get());
    }
};
