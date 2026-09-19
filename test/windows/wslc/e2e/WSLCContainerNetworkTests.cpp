/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerNetworkTests.cpp

Abstract:

    This file contains test cases for WSLC container networking.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerNetworkTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerNetworkTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(ContainerNetwork)
    {
        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLCContainerState>>& expectedContainers) {
            auto [containers, ports] = ListContainers(m_defaultSession.get());
            VERIFY_ARE_EQUAL(expectedContainers.size(), containers.size());

            for (size_t i = 0; i < expectedContainers.size(); i++)
            {
                const auto& [expectedName, expectedImage, expectedState] = expectedContainers[i];
                VERIFY_ARE_EQUAL(expectedName, containers[i].Name);
                VERIFY_ARE_EQUAL(expectedImage, containers[i].Image);
                VERIFY_ARE_EQUAL(expectedState, containers[i].State);
                VERIFY_ARE_EQUAL(strlen(containers[i].Id), WSLC_CONTAINER_ID_LENGTH);
                VERIFY_IS_TRUE(containers[i].StateChangedAt > 0);
                VERIFY_IS_TRUE(containers[i].CreatedAt > 0);
            }
        };

        // Verify that containers launch successfully when host and none are used as network modes
        // TODO: Test bridge network container launch when VHD with bridge cni is ready
        // TODO: Add port mapping related tests when port mapping is implemented
        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "host");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto details = container.Inspect();
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "host");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "none");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "none");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }

        {
            // Unknown network names are rejected as "network not found".
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "no-such-network");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(retVal.first, WSLC_E_NETWORK_NOT_FOUND);
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "bridge");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "bridge");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkTest)
    {
        const std::string networkName = "custom-net-test";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.35.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-custom-network", {"sleep", "99999"}, {}, std::string(networkName));

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkNotFoundTest)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-custom-network-notfound", {"sleep", "99999"}, {}, std::string("nonexistent-net"));

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessageContains(L"nonexistent-net");
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkEmptyNameTest)
    {
        // Empty entry in the Networks array must be rejected.
        LPCSTR args[] = {"sleep", "99999"};
        WSLCNetworkConnection emptyConnection{};
        emptyConnection.NetworkName = ""; // empty name — should be rejected

        WSLCContainerOptions options{};
        options.Image = "debian:latest";
        options.Name = "test-custom-network-empty";
        options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
        options.ContainerNetwork.Networks = &emptyConnection;
        options.ContainerNetwork.NetworksCount = 1;

        wil::com_ptr<IWSLCContainer> container;
        auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
        VERIFY_ARE_EQUAL(E_INVALIDARG, hr);
        ValidateCOMErrorMessageContains(L"Network name");
    }

    WSLC_TEST_METHOD(ContainerNetworkEndpointSettingsUnknownKeyTest)
    {
        // Unknown endpoint setting keys must be rejected at container creation time.
        const std::string networkName = "custom-net-settings-unknown";
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));
        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        LPCSTR args[] = {"sleep", "99999"};
        KeyValuePair settings[] = {{"BogusKey", "value"}};
        WSLCNetworkConnection connection{};
        connection.NetworkName = networkName.c_str();
        connection.Settings = settings;
        connection.SettingsCount = ARRAYSIZE(settings);

        WSLCContainerOptions options{};
        options.Image = "debian:latest";
        options.Name = "test-endpoint-settings-unknown";
        options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
        options.ContainerNetwork.NetworkMode = "bridge";
        options.ContainerNetwork.Networks = &connection;
        options.ContainerNetwork.NetworksCount = 1;

        wil::com_ptr<IWSLCContainer> container;
        auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
        VERIFY_ARE_EQUAL(E_INVALIDARG, hr);
        const auto expectedError =
            std::format(L"Unknown endpoint setting 'BogusKey' for network '{}'.", std::wstring(networkName.begin(), networkName.end()));
        ValidateCOMErrorMessage(expectedError);
    }

    WSLC_TEST_METHOD(ContainerUnsupportedColonNetworkModeRejectedTest)
    {
        // Only `container:` is a recognized colon-prefixed mode. Anything else
        // (`service:foo`, `ns:bar`, ...) must be rejected at the WSLC layer rather
        // than being passed through to Docker or interpreted as a user-defined name.
        WSLCContainerLauncher launcher("debian:latest", "test-unsupported-colon", {"sleep", "99999"}, {}, std::string("service:foo"));

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessageContains(L"service:foo");
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkMultipleContainersTest)
    {
        const std::string networkName = "custom-net-multi";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.36.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher1("debian:latest", "test-custom-multi-1", {"sleep", "99999"}, {}, std::string(networkName));

        WSLCContainerLauncher launcher2("debian:latest", "test-custom-multi-2", {"sleep", "99999"}, {}, std::string(networkName));

        auto container1 = launcher1.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container1.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container1.Inspect().HostConfig.NetworkMode, networkName);

        auto container2 = launcher2.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container2.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container2.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkDeleteWhileInUseTest)
    {
        const std::string networkName = "custom-net-inuse";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.37.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-custom-net-inuse", {"sleep", "99999"}, {}, std::string(networkName));

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), m_defaultSession->DeleteNetwork(networkName.c_str()));
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkPortMappingTest)
    {
        const std::string networkName = "custom-net-ports";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.38.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher(
            "python:3.12-alpine", "test-custom-net-ports", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, std::string(networkName));
        launcher.AddPort(1251, 8000, AF_INET);

        auto container = launcher.Launch(*m_defaultSession);
        auto initProcess = container.GetInitProcess();
        WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

        ExpectHttpResponse(L"http://127.0.0.1:1251", 200);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkRecoveryTest)
    {
        const std::string networkName = "custom-net-recovery";
        const std::string containerName = "test-custom-net-recovery";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.39.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, std::string(networkName));

        {
            auto container = launcher.Create(*m_defaultSession);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
        }

        // Restart the session and verify the container is recovered with its custom network.
        ResetTestSession();

        auto recoveredContainer = OpenContainer(m_defaultSession.get(), containerName);

        VERIFY_ARE_EQUAL(recoveredContainer.State(), WslcContainerStateCreated);
        VERIFY_SUCCEEDED(recoveredContainer.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));
        VERIFY_ARE_EQUAL(recoveredContainer.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(recoveredContainer.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerMultipleNetworksTest)
    {
        const std::string primaryNetwork = "multi-net-primary";
        const std::string additionalNetwork = "multi-net-additional";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));

        WSLCNetworkOptions primaryNetOpts{};
        primaryNetOpts.Name = primaryNetwork.c_str();
        primaryNetOpts.Driver = "bridge";
        primaryNetOpts.Subnet = "172.40.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&primaryNetOpts, nullptr));
        auto primaryCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCNetworkOptions additionalNetOpts{};
        additionalNetOpts.Name = additionalNetwork.c_str();
        additionalNetOpts.Driver = "bridge";
        additionalNetOpts.Subnet = "172.41.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&additionalNetOpts, nullptr));
        auto additionalCleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(additionalNetwork);

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        auto inspect = container.Inspect();
        VERIFY_ARE_EQUAL(inspect.HostConfig.NetworkMode, primaryNetwork);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(primaryNetwork));
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(additionalNetwork));
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(primaryNetwork).IPAddress.empty());
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(additionalNetwork).IPAddress.empty());
    }

    WSLC_TEST_METHOD(ContainerDuplicateNetworkRejectedTest)
    {
        const std::string primaryNetwork = "multi-net-dup-primary";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));

        WSLCNetworkOptions netOpts{};
        netOpts.Name = primaryNetwork.c_str();
        netOpts.Driver = "bridge";
        netOpts.Subnet = "172.48.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-dup-reject", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(primaryNetwork);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(
            std::format(L"Duplicate network: '{}'", std::wstring(primaryNetwork.begin(), primaryNetwork.end())).c_str());
    }

    WSLC_TEST_METHOD(ContainerAdditionalNetworkNotFoundRejectedTest)
    {
        const std::string primaryNetwork = "multi-net-notfound-primary";
        const std::string missingNetwork = "multi-net-nonexistent";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(missingNetwork.c_str()));

        WSLCNetworkOptions netOpts{};
        netOpts.Name = primaryNetwork.c_str();
        netOpts.Driver = "bridge";
        netOpts.Subnet = "172.49.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-notfound-reject", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(missingNetwork);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessage(
            std::format(L"Network not found: '{}'", std::wstring(missingNetwork.begin(), missingNetwork.end())).c_str());
    }

    WSLC_TEST_METHOD(ContainerBridgedPrimaryDuplicateNetworkRejectedTest)
    {
        // Verifies that passing the primary bridge network as an additional network is caught as a duplicate.
        WSLCContainerLauncher launcher("debian:latest", "test-bridge-dup-reject", {"sleep", "99999"}, {}, "bridge");
        launcher.AddAdditionalNetwork("bridge");

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(L"Duplicate network: 'bridge'");
    }

    WSLC_TEST_METHOD(ContainerAdditionalNetworkMalformedNameTest)
    {
        // Most malformed names fall through to the network lookup.

        // Invalid character.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-char", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork("bad/name");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
            ValidateCOMErrorMessage(L"Network not found: 'bad/name'");
        }

        // Empty name.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-empty", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork("");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
            ValidateCOMErrorMessage(L"Network name cannot be empty.");
        }

        // Name exceeds WSLC_MAX_NETWORK_NAME_LENGTH (255).
        {
            const std::string tooLongName(WSLC_MAX_NETWORK_NAME_LENGTH + 1, 'a');

            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-long", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork(tooLongName);

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
            ValidateCOMErrorMessage(std::format(L"Network not found: '{}'", std::wstring(tooLongName.begin(), tooLongName.end())).c_str());
        }
    }

    WSLC_TEST_METHOD(ContainerDeleteAdditionalNetworkWhileInUseTest)
    {
        const std::string primaryNetwork = "multi-net-del-primary";
        const std::string additionalNetwork = "multi-net-del-additional";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));

        WSLCNetworkOptions primaryNetOpts{};
        primaryNetOpts.Name = primaryNetwork.c_str();
        primaryNetOpts.Driver = "bridge";
        primaryNetOpts.Subnet = "172.50.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&primaryNetOpts, nullptr));
        auto primaryCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCNetworkOptions additionalNetOpts{};
        additionalNetOpts.Name = additionalNetwork.c_str();
        additionalNetOpts.Driver = "bridge";
        additionalNetOpts.Subnet = "172.51.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&additionalNetOpts, nullptr));
        auto additionalCleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-del", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(additionalNetwork);

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));
    }

    WSLC_TEST_METHOD(ConnectDisconnectContainerNetworkTest)
    {
        auto launchContainer = [&](const std::string& name, std::string networkMode = "bridge") {
            WSLCContainerLauncher launcher("debian:latest", name, {"sleep", "99999"}, {}, std::move(networkMode));
            return launcher.Launch(*m_defaultSession);
        };

        auto createNetwork = [&](const std::string& name, const char* subnet) {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(name.c_str()));
            WSLCNetworkOptions netOpts{};
            netOpts.Name = name.c_str();
            netOpts.Driver = "bridge";
            netOpts.Subnet = subnet;
            VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        };

        // Verifies both ConnectToNetwork and DisconnectFromNetwork reject with the same error for the given network name.
        auto expectBothReject = [&](auto& container, LPCSTR networkName, HRESULT hr, LPCWSTR message) {
            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName;
            VERIFY_ARE_EQUAL(hr, container.Get().ConnectToNetwork(&options));
            ValidateCOMErrorMessage(message);
            VERIFY_ARE_EQUAL(hr, container.Get().DisconnectFromNetwork(networkName));
            ValidateCOMErrorMessage(message);
        };

        // Round-trip: connect to a network, verify via inspect, then disconnect.
        {
            const std::string networkName = "test-connect-disconnect-net";
            createNetwork(networkName, "172.53.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchContainer("test-connect-disconnect-ctr");

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(networkName).IPAddress.empty());

            VERIFY_SUCCEEDED(container.Get().DisconnectFromNetwork(networkName.c_str()));

            auto inspectAfter = container.Inspect();
            VERIFY_IS_FALSE(inspectAfter.NetworkSettings.Networks.contains(networkName));
            VERIFY_IS_TRUE(inspectAfter.NetworkSettings.Networks.contains("bridge"));
        }

        // Empty and non-existent network name.
        {
            auto container = launchContainer("test-connect-invalid-name");

            expectBothReject(container, "", E_INVALIDARG, L"Network name cannot be empty.");

            const std::string nonExistentNetwork = "nonexistent-network";
            const auto expectedError =
                std::format(L"Network not found: '{}'", std::wstring(nonExistentNetwork.begin(), nonExistentNetwork.end()));
            expectBothReject(container, nonExistentNetwork.c_str(), WSLC_E_NETWORK_NOT_FOUND, expectedError.c_str());
        }

        // Host, none, and container:* mode rejection.
        {
            const std::string networkName = "test-connect-mode-net";
            createNetwork(networkName, "172.52.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto expectModeRejection = [&](const std::string& name, const std::string& mode) {
                auto container = launchContainer(name, mode);
                const auto expected = std::format(
                    L"The primary network mode '{}' does not support connecting or disconnecting additional networks.",
                    std::wstring(mode.begin(), mode.end()));
                expectBothReject(container, networkName.c_str(), E_INVALIDARG, expected.c_str());
            };

            expectModeRejection("test-connect-host-ctr", "host");
            expectModeRejection("test-connect-none-ctr", "none");

            // container:<name> is resolved to container:<id> internally, so the emitted mode string
            // contains the target's container ID rather than its name. Verify HRESULT + a stable substring.
            const std::string ctrModeTarget = "test-connect-ctrmode-target";
            auto target = launchContainer(ctrModeTarget);
            auto ctrModeContainer = launchContainer("test-connect-ctrmode-ctr", "container:" + ctrModeTarget);
            const std::wstring expectedSubstring = L"does not support connecting or disconnecting additional networks";

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            VERIFY_ARE_EQUAL(E_INVALIDARG, ctrModeContainer.Get().ConnectToNetwork(&options));
            ValidateCOMErrorMessageContains(expectedSubstring);
            VERIFY_ARE_EQUAL(E_INVALIDARG, ctrModeContainer.Get().DisconnectFromNetwork(networkName.c_str()));
            ValidateCOMErrorMessageContains(expectedSubstring);
        }

        // Connect and disconnect from the container's primary network.
        {
            const std::string containerName = "test-connect-primary-ctr";
            auto container = launchContainer(containerName);

            // Connect to primary should fail — already connected.
            WSLCNetworkConnectionOptions options{};
            options.NetworkName = "bridge";
            VERIFY_ARE_EQUAL(E_FAIL, container.Get().ConnectToNetwork(&options));
            // Docker returns the container name in the error, not the ID.
            const auto expectedError = std::format(
                L"endpoint with name {} already exists in network bridge", std::wstring(containerName.begin(), containerName.end()));
            ValidateCOMErrorMessage(expectedError);

            // Disconnect from primary should succeed.
            VERIFY_SUCCEEDED(container.Get().DisconnectFromNetwork("bridge"));

            auto inspect = container.Inspect();
            VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.contains("bridge"));
        }

        // Connect to same secondary network twice.
        {
            const std::string networkName = "test-connect-twice-net";
            const std::string containerName = "test-connect-twice-ctr";
            createNetwork(networkName, "172.51.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchContainer(containerName);

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));
            VERIFY_ARE_EQUAL(E_FAIL, container.Get().ConnectToNetwork(&options));
            // Docker returns the container name in the error, not the ID.
            const auto expectedError = std::format(
                L"endpoint with name {} already exists in network {}",
                std::wstring(containerName.begin(), containerName.end()),
                std::wstring(networkName.begin(), networkName.end()));
            ValidateCOMErrorMessage(expectedError);
        }
    }

    WSLC_TEST_METHOD(NetworkConnectEndpointSettingsTest)
    {
        const std::string networkName = "connect-endpoint-net";
        const std::string subnet = "172.70.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        WSLCNetworkOptions netOpts{};
        netOpts.Name = networkName.c_str();
        netOpts.Driver = "bridge";
        netOpts.Subnet = subnet.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        auto launchContainer = [&](const std::string& name) {
            WSLCContainerLauncher launcher("debian:latest", name, {"sleep", "99999"}, {}, "bridge");
            return launcher.Launch(*m_defaultSession);
        };

        auto expectConnectReject =
            [&](auto& container, const std::vector<KeyValuePair>& settings, HRESULT expectedResult, const std::wstring& expectedMessage) {
                WSLCNetworkConnectionOptions options{};
                options.NetworkName = networkName.c_str();
                options.Settings = settings.data();
                options.SettingsCount = static_cast<ULONG>(settings.size());
                VERIFY_ARE_EQUAL(expectedResult, container.Get().ConnectToNetwork(&options));
                ValidateCOMErrorMessage(expectedMessage);
            };

        // Unknown endpoint setting key rejected.
        {
            auto container = launchContainer("connect-endpoint-unknown");
            const std::string unknownKey = "BogusKey";
            const std::string unknownValue = "value";
            const auto expected = std::format(
                L"Unknown endpoint setting '{}' for network '{}'.",
                std::wstring(unknownKey.begin(), unknownKey.end()),
                std::wstring(networkName.begin(), networkName.end()));
            expectConnectReject(container, {{unknownKey.c_str(), unknownValue.c_str()}}, E_INVALIDARG, expected);
        }

        // Malformed IPv4 address rejected.
        {
            auto container = launchContainer("connect-endpoint-badip");
            const std::string badIp = "not-an-ip";
            const auto expected = std::format(L"Invalid IP address '{}'", std::wstring(badIp.begin(), badIp.end()));
            expectConnectReject(container, {{"IPAddress", badIp.c_str()}}, E_INVALIDARG, expected);
        }

        // Multiple IPAddress values rejected.
        {
            auto container = launchContainer("connect-endpoint-dupip");
            const std::string firstIp = "172.70.0.5";
            const std::string secondIp = "172.70.0.6";
            const auto expected = std::format(
                L"Only one IP address may be specified for network '{}'.", std::wstring(networkName.begin(), networkName.end()));
            expectConnectReject(container, {{"IPAddress", firstIp.c_str()}, {"IPAddress", secondIp.c_str()}}, E_INVALIDARG, expected);
        }

        // Empty link rejected.
        {
            auto container = launchContainer("connect-endpoint-emptylink");
            expectConnectReject(container, {{"Links", ""}}, E_INVALIDARG, L"Network link cannot be empty.");
        }

        // Malformed driver option (missing '=') rejected.
        {
            auto container = launchContainer("connect-endpoint-badopt");
            const std::string badEntry = "no-equals";
            const auto expected =
                std::format(L"Invalid driver option '{}'; expected 'key=value'.", std::wstring(badEntry.begin(), badEntry.end()));
            expectConnectReject(container, {{"DriverOpts", badEntry.c_str()}}, E_INVALIDARG, expected);
        }

        // Duplicate driver option key rejected.
        {
            auto container = launchContainer("connect-endpoint-dupopt");
            const std::string dupKey = "mtu";
            const std::string firstEntry = "mtu=1500";
            const std::string secondEntry = "mtu=1400";
            const auto expected = std::format(L"Duplicate driver option '{}'.", std::wstring(dupKey.begin(), dupKey.end()));
            expectConnectReject(container, {{"DriverOpts", firstEntry.c_str()}, {"DriverOpts", secondEntry.c_str()}}, E_INVALIDARG, expected);
        }

        // Malformed link-local IPv4 address rejected.
        {
            auto container = launchContainer("connect-endpoint-badlli");
            const std::string badIp = "not-a-link-local";
            const auto expected = std::format(L"Invalid IP address '{}'", std::wstring(badIp.begin(), badIp.end()));
            expectConnectReject(container, {{"LinkLocalIPs", badIp.c_str()}}, E_INVALIDARG, expected);
        }

        // Empty/whitespace-only alias rejected.
        {
            auto container = launchContainer("connect-endpoint-emptyalias");
            expectConnectReject(container, {{"Aliases", "   "}}, E_INVALIDARG, L"Network alias cannot be empty.");
        }

        // Driver option with empty key ('=value') rejected.
        {
            auto container = launchContainer("connect-endpoint-emptykey");
            const std::string badEntry = "=value";
            const auto expected =
                std::format(L"Invalid driver option '{}'; expected 'key=value'.", std::wstring(badEntry.begin(), badEntry.end()));
            expectConnectReject(container, {{"DriverOpts", badEntry.c_str()}}, E_INVALIDARG, expected);
        }

        // Driver option value containing '=' — everything after the first '=' is preserved verbatim.
        {
            auto container = launchContainer("connect-endpoint-eqvalue");
            const std::string driverOptKey = "com.docker.network.endpoint.custom";
            const std::string driverOptValue = "a=b=c";
            const std::string driverOptEntry = driverOptKey + "=" + driverOptValue;

            const std::vector<KeyValuePair> settings{{"DriverOpts", driverOptEntry.c_str()}};
            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            options.Settings = settings.data();
            options.SettingsCount = static_cast<ULONG>(settings.size());
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            const auto opt = endpoint.DriverOpts.find(driverOptKey);
            VERIFY_IS_TRUE(opt != endpoint.DriverOpts.end());
            VERIFY_ARE_EQUAL(driverOptValue, opt->second);
        }

        // Aliases + IPAddress + LinkLocalIPs + DriverOpts together — success, inspect verifies every field.
        {
            auto container = launchContainer("connect-endpoint-combo");
            const std::string alias1 = "primary-alias";
            const std::string alias2 = "secondary-alias";
            const std::string ipAddress = "172.70.0.42";
            const std::string linkLocal = "169.254.10.5";
            const std::string driverOptEntry = "com.docker.network.endpoint.custom=verify";
            const std::string driverOptKey = "com.docker.network.endpoint.custom";
            const std::string driverOptValue = "verify";

            const std::vector<KeyValuePair> settings{
                {"Aliases", alias1.c_str()},
                {"Aliases", alias2.c_str()},
                {"IPAddress", ipAddress.c_str()},
                {"LinkLocalIPs", linkLocal.c_str()},
                {"DriverOpts", driverOptEntry.c_str()},
            };

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            options.Settings = settings.data();
            options.SettingsCount = static_cast<ULONG>(settings.size());
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias1) != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias2) != endpoint.Aliases.end());
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAddress);
            VERIFY_IS_TRUE(endpoint.IPAMConfig.has_value());
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAMConfig->IPv4Address);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.IPAMConfig->LinkLocalIPs, linkLocal) != endpoint.IPAMConfig->LinkLocalIPs.end());
            const auto opt = endpoint.DriverOpts.find(driverOptKey);
            VERIFY_IS_TRUE(opt != endpoint.DriverOpts.end());
            VERIFY_ARE_EQUAL(driverOptValue, opt->second);
        }

        // Links: connect two containers to the same user-defined bridge, verify Links echoes back in inspect.
        {
            const std::string targetName = "connect-endpoint-link-target";
            const std::string targetAlias = "db";
            auto target = launchContainer(targetName);
            WSLCNetworkConnectionOptions targetOptions{};
            targetOptions.NetworkName = networkName.c_str();
            const std::vector<KeyValuePair> targetSettings{{"Aliases", targetAlias.c_str()}};
            targetOptions.Settings = targetSettings.data();
            targetOptions.SettingsCount = static_cast<ULONG>(targetSettings.size());
            VERIFY_SUCCEEDED(target.Get().ConnectToNetwork(&targetOptions));

            auto source = launchContainer("connect-endpoint-link-source");
            const std::string linkEntry = targetName + ":" + targetAlias;
            const std::vector<KeyValuePair> sourceSettings{{"Links", linkEntry.c_str()}};
            WSLCNetworkConnectionOptions sourceOptions{};
            sourceOptions.NetworkName = networkName.c_str();
            sourceOptions.Settings = sourceSettings.data();
            sourceOptions.SettingsCount = static_cast<ULONG>(sourceSettings.size());
            VERIFY_SUCCEEDED(source.Get().ConnectToNetwork(&sourceOptions));

            auto inspect = source.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Links, linkEntry) != endpoint.Links.end());
        }

        // Connecting a stopped container succeeds — Docker allows attach in created/exited state.
        {
            auto container = launchContainer("connect-endpoint-stopped");
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            const std::string alias = "stopped-alias";
            const std::vector<KeyValuePair> settings{{"Aliases", alias.c_str()}};

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            options.Settings = settings.data();
            options.SettingsCount = static_cast<ULONG>(settings.size());
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias) != endpoint.Aliases.end());
        }
    }

    WSLC_TEST_METHOD(NetworkAliasCreateTest)
    {
        auto createNetwork = [&](const std::string& name, const char* subnet) {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(name.c_str()));
            WSLCNetworkOptions netOpts{};
            netOpts.Name = name.c_str();
            netOpts.Driver = "bridge";
            netOpts.Subnet = subnet;
            VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        };

        auto launchWithAliases = [&](const std::string& containerName, const std::string& networkMode, const std::vector<std::string>& aliases) {
            WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, networkMode);
            for (const auto& a : aliases)
            {
                launcher.AddPrimaryNetworkAlias(a);
            }
            return launcher.Launch(*m_defaultSession);
        };

        auto expectError = [&](const std::string& containerName,
                               const std::string& networkMode,
                               const std::vector<std::string>& aliases,
                               HRESULT expectedResult,
                               const std::wstring& expectedErrorMessage) {
            auto result = wil::ResultFromException([&] { launchWithAliases(containerName, networkMode, aliases); });
            VERIFY_ARE_EQUAL(result, expectedResult);
            ValidateCOMErrorMessage(expectedErrorMessage);
        };

        // Single user-defined network + single alias — success, round-trips via inspect.
        {
            const std::string networkName = "alias-net-single";
            createNetwork(networkName, "172.60.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchWithAliases("alias-ctr-single", networkName, {"db"});

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
        }

        // Multiple aliases on a single network — all present.
        {
            const std::string networkName = "alias-net-multi";
            createNetwork(networkName, "172.61.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchWithAliases("alias-ctr-multi", networkName, {"db", "primary", "backup"});

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "primary") != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "backup") != endpoint.Aliases.end());
        }

        // Aliases on primary and additional user-defined networks — all present.
        {
            const std::string primaryNetworkName = "alias-net-primary";
            const std::string additionalNetworkName = "alias-net-additional";
            createNetwork(primaryNetworkName, "172.64.0.0/16");
            createNetwork(additionalNetworkName, "172.65.0.0/16");
            auto primaryNetCleanup =
                wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetworkName.c_str())); });
            auto additionalNetCleanup =
                wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetworkName.c_str())); });

            WSLCContainerLauncher launcher("debian:latest", "alias-ctr-additional", {"sleep", "99999"}, {}, primaryNetworkName);
            launcher.AddPrimaryNetworkAlias("db");
            launcher.AddAdditionalNetwork(additionalNetworkName, {"cache", "replica"});
            auto container = launcher.Launch(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(primaryNetworkName));
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(additionalNetworkName));
            const auto& primaryEndpoint = inspect.NetworkSettings.Networks.at(primaryNetworkName);
            const auto& additionalEndpoint = inspect.NetworkSettings.Networks.at(additionalNetworkName);
            VERIFY_IS_TRUE(std::ranges::find(primaryEndpoint.Aliases, "db") != primaryEndpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(additionalEndpoint.Aliases, "cache") != additionalEndpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(additionalEndpoint.Aliases, "replica") != additionalEndpoint.Aliases.end());
        }

        // Aliases on additional built-in/non-user-defined networks — rejected before network lookup.
        {
            const std::string primaryNetworkName = "alias-net-invalid-additional";
            createNetwork(primaryNetworkName, "172.66.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetworkName.c_str())); });

            auto expectAdditionalNetworkAliasError = [&](const std::string& containerName, const std::string& additionalNetworkName) {
                WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, primaryNetworkName);
                launcher.AddAdditionalNetwork(additionalNetworkName, {"db"});

                auto result = wil::ResultFromException([&] { launcher.Launch(*m_defaultSession); });
                VERIFY_ARE_EQUAL(E_INVALIDARG, result);
                ValidateCOMErrorMessage(L"Network aliases require a user-defined network. Use --network to specify one.");
            };

            expectAdditionalNetworkAliasError("alias-ctr-additional-bridge", "bridge");
            expectAdditionalNetworkAliasError("alias-ctr-additional-host", "host");
            expectAdditionalNetworkAliasError("alias-ctr-additional-none", "none");
            expectAdditionalNetworkAliasError("alias-ctr-additional-container", "container:alias-ctr-target");
        }

        // Alias on 'host' mode — rejected at the IDL layer.
        {
            expectError(
                "alias-ctr-host",
                "host",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on default 'bridge' mode — rejected (aliases require a user-defined network).
        {
            expectError(
                "alias-ctr-bridge",
                "bridge",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on 'none' mode — rejected.
        {
            expectError(
                "alias-ctr-none",
                "none",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on 'container:' mode — rejected.
        {
            WSLCContainerLauncher targetLauncher("debian:latest", "alias-ctr-target", {"sleep", "99999"}, {});
            auto target = targetLauncher.Launch(*m_defaultSession);

            expectError(
                "alias-ctr-container",
                "container:alias-ctr-target",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Empty alias string — rejected.
        {
            const std::string networkName = "alias-net-empty";
            createNetwork(networkName, "172.62.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            expectError("alias-ctr-empty", networkName, {""}, E_INVALIDARG, L"Network alias cannot be empty.");
        }

        // Unknown KVP key on primary settings — rejected with the unified endpoint-settings error.
        {
            const std::string networkName = "alias-net-unknown";
            const std::string unknownKey = "BogusKey";
            const std::string unknownValue = "value";
            createNetwork(networkName, "172.63.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            LPCSTR args[] = {"sleep", "99999"};
            const KeyValuePair settings[] = {{unknownKey.c_str(), unknownValue.c_str()}};
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "alias-ctr-unknown";
            options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
            options.ContainerNetwork.NetworkMode = networkName.c_str();
            options.ContainerNetwork.Settings = settings;
            options.ContainerNetwork.SettingsCount = ARRAYSIZE(settings);

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateContainer(&options, nullptr, &container));
            ValidateCOMErrorMessage(std::format(
                L"Unknown endpoint setting '{}' for network '{}'.",
                std::wstring(unknownKey.begin(), unknownKey.end()),
                std::wstring(networkName.begin(), networkName.end())));
        }

        // Primary endpoint settings (IPAddress + Aliases + LinkLocalIPs + DriverOpts) round-trip via inspect at create time.
        {
            const std::string networkName = "alias-net-combo";
            createNetwork(networkName, "172.64.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            const std::string alias1 = "primary-alias";
            const std::string alias2 = "secondary-alias";
            const std::string ipAddress = "172.64.0.42";
            const std::string linkLocal = "169.254.11.5";
            const std::string driverOptEntry = "com.docker.network.endpoint.custom=verify";
            const std::string driverOptKey = "com.docker.network.endpoint.custom";
            const std::string driverOptValue = "verify";

            LPCSTR args[] = {"sleep", "99999"};
            const KeyValuePair settings[] = {
                {"Aliases", alias1.c_str()},
                {"Aliases", alias2.c_str()},
                {"IPAddress", ipAddress.c_str()},
                {"LinkLocalIPs", linkLocal.c_str()},
                {"DriverOpts", driverOptEntry.c_str()},
            };
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "alias-ctr-combo";
            options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
            options.ContainerNetwork.NetworkMode = networkName.c_str();
            options.ContainerNetwork.Settings = settings;
            options.ContainerNetwork.SettingsCount = ARRAYSIZE(settings);

            wil::com_ptr<IWSLCContainer> containerCom;
            VERIFY_SUCCEEDED(m_defaultSession->CreateContainer(&options, nullptr, &containerCom));
            RunningWSLCContainer container(std::move(containerCom), WSLCProcessFlagsNone);
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias1) != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias2) != endpoint.Aliases.end());
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAddress);
            VERIFY_IS_TRUE(endpoint.IPAMConfig.has_value());
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAMConfig->IPv4Address);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.IPAMConfig->LinkLocalIPs, linkLocal) != endpoint.IPAMConfig->LinkLocalIPs.end());
            const auto opt = endpoint.DriverOpts.find(driverOptKey);
            VERIFY_IS_TRUE(opt != endpoint.DriverOpts.end());
            VERIFY_ARE_EQUAL(driverOptValue, opt->second);
        }

        // Launcher-driven pinned IP alongside an alias — both settings survive the same KVP batch.
        {
            const std::string networkName = "alias-net-ip";
            const std::string ipAddress = "172.67.0.42";
            const std::string alias = "db";
            createNetwork(networkName, "172.67.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            WSLCContainerLauncher launcher("debian:latest", "alias-ctr-ip", {"sleep", "99999"}, {}, networkName);
            launcher.AddPrimaryNetworkAlias(alias);
            launcher.SetPrimaryNetworkIpAddress(std::string(ipAddress));
            auto container = launcher.Launch(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAddress);
            VERIFY_IS_TRUE(endpoint.IPAMConfig.has_value());
            VERIFY_ARE_EQUAL(ipAddress, endpoint.IPAMConfig->IPv4Address);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, alias) != endpoint.Aliases.end());
        }

        // Pinned IP outside a user-defined network — rejected for callers that bypass the CLI checks.
        {
            auto expectEndpointSettingsError = [&](const std::string& containerName, const std::string& networkMode) {
                WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, networkMode);
                launcher.SetPrimaryNetworkIpAddress("172.67.0.42");

                auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
                VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
                ValidateCOMErrorMessage(std::format(
                    L"Endpoint settings are not supported for network mode '{}'.", std::wstring(networkMode.begin(), networkMode.end())));
            };

            expectEndpointSettingsError("alias-ctr-ip-host", "host");
            expectEndpointSettingsError("alias-ctr-ip-none", "none");
        }

        // Primary endpoint Links: launch a target container with an alias, then a source with --link at create time.
        {
            const std::string networkName = "alias-net-link";
            const std::string targetName = "alias-ctr-link-target";
            const std::string targetAlias = "db";
            createNetwork(networkName, "172.65.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto target = launchWithAliases(targetName, networkName, {targetAlias});

            const std::string linkEntry = targetName + ":" + targetAlias;
            LPCSTR args[] = {"sleep", "99999"};
            const KeyValuePair settings[] = {{"Links", linkEntry.c_str()}};
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "alias-ctr-link-source";
            options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
            options.ContainerNetwork.NetworkMode = networkName.c_str();
            options.ContainerNetwork.Settings = settings;
            options.ContainerNetwork.SettingsCount = ARRAYSIZE(settings);

            wil::com_ptr<IWSLCContainer> sourceCom;
            VERIFY_SUCCEEDED(m_defaultSession->CreateContainer(&options, nullptr, &sourceCom));
            RunningWSLCContainer source(std::move(sourceCom), WSLCProcessFlagsNone);
            VERIFY_SUCCEEDED(source.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));

            auto inspect = source.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Links, linkEntry) != endpoint.Links.end());
        }
    }

    WSLC_TEST_METHOD(ContainerNetworkModeHappyPathTest)
    {
        // Start container A on the default (bridged) network, then start container B sharing A's
        // network namespace. Verify the inspect round-trip returns the canonical container mode.
        const std::string containerAName = "test-container-mode-a";
        const std::string containerBName = "test-container-mode-b";

        WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
        auto containerA = launcherA.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);

        const std::string containerAId = containerA.Id();

        WSLCContainerLauncher launcherB("debian:latest", containerBName, {"sleep", "99999"}, {}, "container:" + containerAName);

        auto containerB = launcherB.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerB.State(), WslcContainerStateRunning);

        // Inspect B: NetworkMode must be "container:<A's canonical 64-char id>".
        const std::string expectedNetworkMode = "container:" + containerAId;
        VERIFY_ARE_EQUAL(containerB.Inspect().HostConfig.NetworkMode, expectedNetworkMode);
    }

    WSLC_TEST_METHOD(ContainerNetworkModeMissingTargetRejectedTest)
    {
        // Container mode with an empty target name must be rejected before any Docker call.
        WSLCContainerLauncher launcher("debian:latest", "test-container-mode-no-target", {"sleep", "99999"}, {}, "container:");

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(L"Target container name is required for container network mode.");
    }

    WSLC_TEST_METHOD(ContainerNetworkModeTargetNotFoundTest)
    {
        // Container mode with a nonexistent target must return WSLC_E_CONTAINER_NOT_FOUND
        // with a localized message naming the target.
        const std::string targetName = "does-not-exist-container-target";

        WSLCContainerLauncher launcher("debian:latest", "test-container-mode-notfound", {"sleep", "99999"}, {}, "container:" + targetName);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessage(std::format(L"Target container '{}' not found.", targetName));
    }

    WSLC_TEST_METHOD(ContainerNetworkModePortsRejectedTest)
    {
        // Container mode does not support port mappings — ports belong to the target container.
        const std::string containerAName = "test-container-mode-ports-a";

        WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
        auto containerA = launcherA.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);

        WSLCContainerLauncher launcherB("debian:latest", "test-container-mode-ports-b", {"sleep", "99999"}, {}, "container:" + containerAName);
        launcherB.AddPort(8080, 80, AF_INET);

        auto retVal = launcherB.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(
            L"Port mappings are not supported with container network mode; ports are owned by the target container.");
    }

    WSLC_TEST_METHOD(ContainerNetworkModeInspectRoundTripTest)
    {
        // Verify that after a session reset (service restart), Inspect() on a recovered
        // container-mode container still returns the correct "container:<id>" NetworkMode.
        const std::string containerAName = "test-container-mode-rt-a";
        const std::string containerBName = "test-container-mode-rt-b";

        std::string containerAId;

        {
            WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
            auto containerA = launcherA.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);
            containerAId = containerA.Id();
            containerA.SetDeleteOnClose(false);

            WSLCContainerLauncher launcherB("debian:latest", containerBName, {"sleep", "99999"}, {}, "container:" + containerAName);
            auto containerB = launcherB.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(containerB.State(), WslcContainerStateCreated);
            containerB.SetDeleteOnClose(false);
        }

        // Simulate service restart — Open() path reconstructs container from Docker state.
        ResetTestSession();

        auto recoveredContainerA = OpenContainer(m_defaultSession.get(), containerAName);
        auto recoveredContainerB = OpenContainer(m_defaultSession.get(), containerBName);
        VERIFY_ARE_EQUAL(recoveredContainerB.State(), WslcContainerStateCreated);

        const std::string expectedNetworkMode = "container:" + containerAId;
        VERIFY_ARE_EQUAL(recoveredContainerB.Inspect().HostConfig.NetworkMode, expectedNetworkMode);
    }
};
