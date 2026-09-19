/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCRecoveryTests.cpp

Abstract:

    This file contains test cases for WSLC session and container recovery.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCRecoveryTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCRecoveryTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(ContainerRecoveryFromStorage)
    {
        auto restore = ResetTestSession(); // Required to access the storage folder.

        std::string containerName = "test-container";
        LONGLONG originalStateChangedAt{};
        LONGLONG originalCreatedAt{};

        // Phase 1: Create session and container, then stop the container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Create and start a container
            WSLCContainerLauncher launcher("debian:latest", containerName.c_str(), {"sleep", "9999"});

            auto container = launcher.Launch(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Stop the container so it can be recovered and deleted later
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Capture StateChangedAt and CreatedAt before the session is destroyed.
            auto [containers, ports] = ListContainers(session.get());
            VERIFY_ARE_EQUAL(containers.size(), 1);
            originalStateChangedAt = containers[0].StateChangedAt;
            originalCreatedAt = containers[0].CreatedAt;
            VERIFY_IS_TRUE(originalStateChangedAt > 0);
            VERIFY_IS_TRUE(originalCreatedAt > 0);
        }

        // Phase 2: Create new session from same storage, recover and delete container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            auto container = OpenContainer(session.get(), containerName);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that StateChangedAt was correctly restored from the Docker timestamp.
            auto [containers, ports] = ListContainers(session.get());
            VERIFY_ARE_EQUAL(containers.size(), 1);

            // StateChangedAt may differ by ~1s between live (event time) and recovery (FinishedAt).
            auto stateChangedAtDiff = (containers[0].StateChangedAt > originalStateChangedAt)
                                          ? (containers[0].StateChangedAt - originalStateChangedAt)
                                          : (originalStateChangedAt - containers[0].StateChangedAt);
            VERIFY_IS_TRUE(stateChangedAtDiff <= 60);
            VERIFY_ARE_EQUAL(containers[0].CreatedAt, originalCreatedAt);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Phase 3: Create new session from same storage, verify the container is not listed.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeAndPortRecoveryFromStorage)
    {
        auto restore = ResetTestSession();

        std::string containerName = "test-recovery-volumes-ports";

        auto hostFolder = std::filesystem::current_path() / "test-recovery-volume";
        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
        });

        // Create a test file in the host folder
        std::ofstream testFile(hostFolder / "test.txt");
        testFile << "recovery-test-content";
        testFile.close();

        // Create session and container with volumes and ports (but don't start it)
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));

            WSLCContainerLauncher launcher(
                "python:3.12-alpine",
                containerName,
                {"python3", "-m", "http.server", "--directory", "/volume"},
                {"PYTHONUNBUFFERED=1"},
                "bridge");

            launcher.AddPort(1250, 8000, AF_INET);
            launcher.AddVolume(hostFolder.wstring(), "/volume", false);

            // Create container but don't start it
            auto container = launcher.Create(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
        }

        // Recover the container in a new session, start it and verify volume and port mapping works.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));
            auto container = OpenContainer(session.get(), containerName);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));

            auto initProcess = container.GetInitProcess();
            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on 0.0.0.0 port 8000");

            // A 200 response also indicates the test file is available so volume was mounted correctly.
            ExpectHttpResponse(L"http://127.0.0.1:1250/test.txt", 200);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Delete the host folder to simulate volume folder being missing on recovery
        cleanup.reset();

        // Create a new session - this should succeed even though the volume folder is gone
        auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));

        wil::com_ptr<IWSLCContainer> container;
        auto hr = session->OpenContainer(containerName.c_str(), &container);

        VERIFY_ARE_EQUAL(hr, WSLC_E_CONTAINER_NOT_FOUND);
    }

    TEST_METHOD(ContainerRecoveryFromStorageInvalidMetadata)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "container", "rm", "-f", "test-invalid-metadata"});
        });

        {
            // Create a docker container that has no metadata.
            auto result = RunCommand(
                m_defaultSession.get(),
                {"/usr/bin/docker", "container", "create", "--name", "test-invalid-metadata", "debian:latest"});
            VERIFY_ARE_EQUAL(result.Code, 0L);
        }

        {
            ResetTestSession();
            // Try to open the container - this should fail due to missing metadata.
            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->OpenContainer("test-invalid-metadata", &container);
            VERIFY_ARE_EQUAL(hr, E_UNEXPECTED);
        }
    }

    WSLC_TEST_METHOD(SessionManagement)
    {
        auto manager = OpenSessionManager();

        auto expectSessions = [&](const std::vector<std::wstring>& expectedSessions) {
            auto displayNames = ListTestSessionNames(manager.get());

            for (const auto& e : expectedSessions)
            {
                auto it = displayNames.find(e);
                if (it == displayNames.end())
                {
                    LogError("Session not found: %ls", e.c_str());
                    VERIFY_FAIL();
                }

                displayNames.erase(it);
            }

            for (const auto& e : displayNames)
            {
                LogError("Unexpected session found: %ls", e.c_str());
                VERIFY_FAIL();
            }
        };

        // Persistent sessions outlive the COM reference that created them, so a test that fails
        // partway through would leave them behind for the next run to trip over. Terminate the ones
        // this test created, however it exits.
        std::set<std::wstring> persistentSessions;
        auto terminatePersistentSessions = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            for (const auto& name : persistentSessions)
            {
                wil::com_ptr<IWSLCSession> session;
                if (SUCCEEDED(manager->OpenSessionByName(name.c_str(), &session)))
                {
                    LOG_IF_FAILED(session->Terminate());
                }
            }
        });

        auto create = [&](LPCWSTR Name, WSLCSessionFlags Flags) {
            if (WI_IsFlagSet(Flags, WSLCSessionFlagsPersistent))
            {
                persistentSessions.emplace(Name);
            }

            return CreateSession(GetDefaultSessionSettings(Name), Flags);
        };

        // Validate that non-persistent sessions are dropped when released
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsNone);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that persistent sessions are only dropped when explicitly terminated.
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});
            session1 = create(L"session-1", WSLCSessionFlagsOpenExisting);

            VERIFY_SUCCEEDED(session1->Terminate());
            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that sessions can be reopened by name.
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});

            auto session1Copy =
                create(L"session-1", static_cast<WSLCSessionFlags>(WSLCSessionFlagsPersistent | WSLCSessionFlagsOpenExisting));

            expectSessions({L"session-1", c_testSessionName});

            // Verify that name conflicts are correctly handled.
            auto settings = GetDefaultSessionSettings(L"session-1");

            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(manager->CreateSession(&settings, WSLCSessionFlagsPersistent, nullptr, &session), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(session1Copy->Terminate());
            WSLCSessionState state{};
            VERIFY_SUCCEEDED(session1Copy->GetState(&state));
            VERIFY_ARE_EQUAL(state, WSLCSessionStateTerminated);
            expectSessions({c_testSessionName});

            // Validate that a new session is created if WSLCSessionFlagsOpenExisting is set and no match is found.
            auto session2 = create(L"session-2", static_cast<WSLCSessionFlags>(WSLCSessionFlagsOpenExisting));
        }

        // Validate that elevated session can't be opened by non-elevated tokens
        {
            auto elevatedSession = create(L"elevated-session", WSLCSessionFlagsNone);

            auto nonElevatedToken = GetNonElevatedToken(TokenImpersonation);
            auto revert = wil::impersonate_token(nonElevatedToken.get());
            auto nonElevatedSession = create(L"non-elevated-session", WSLCSessionFlagsNone);

            // Validate that non-elevated tokens can't open an elevated session.
            wil::com_ptr<IWSLCSession> openedSession;
            ULONG elevatedId{};
            VERIFY_SUCCEEDED(elevatedSession->GetId(&elevatedId));
            VERIFY_ARE_EQUAL(manager->OpenSession(elevatedId, &openedSession), HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED));
            VERIFY_IS_FALSE(!!openedSession);

            // Validate that non-elevated tokens can open non-elevated sessions.
            ULONG nonElevatedId{};
            VERIFY_SUCCEEDED(nonElevatedSession->GetId(&nonElevatedId));
            VERIFY_SUCCEEDED(manager->OpenSession(nonElevatedId, &openedSession));
            VERIFY_IS_TRUE(!!openedSession);
        }
    }
};
