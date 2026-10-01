/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCPortMappingTests.cpp

Abstract:

    This file contains test cases for WSLC container port mappings.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCPortMappingTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCPortMappingTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    void RunPortMappingsTest(IWSLCSession& session, const std::string& containerNetworkType, bool virtionet)
    {
        WEX::Logging::Log::Comment(
            std::format(L"Container network type: {}", wsl::shared::string::MultiByteToWide(containerNetworkType)).c_str());

        auto expectBoundPorts = [&](RunningWSLCContainer& Container, const std::vector<std::string>& expectedBoundPorts) {
            auto ports = Container.Inspect().Ports;

            std::vector<std::string> boundPorts;
            for (const auto& e : ports)
            {
                boundPorts.emplace_back(e.first);
            }

            if (!std::ranges::equal(boundPorts, expectedBoundPorts))
            {
                LogError(
                    "Port bindings do not match expected values. Expected: [%hs], Actual: [%hs]",
                    wsl::shared::string::Join(expectedBoundPorts, ',').c_str(),
                    wsl::shared::string::Join(boundPorts, ',').c_str());

                VERIFY_FAIL();
            }
        };

        // Test a simple port mapping.
        {
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports", {"python3", "-m", "http.server", "--bind", "::"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6, IPPROTO_TCP, "::1");

            auto container = launcher.Launch(session);
            auto initProcess = container.GetInitProcess();

            // Wait for the container bind() to be completed.
            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

            expectBoundPorts(container, {"8000/tcp"});

            ExpectHttpResponse(L"http://127.0.0.1:1234", 200);

            ExpectHttpResponse(L"http://[::1]:1234", 200);

            // Verify that ListContainers returns the port data for a running container.
            {
                wsl::windows::common::wslc::unique_container_entry_array containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                // Find the container ID for "test-ports"
                std::string testPortsId;
                for (const auto& entry : containers)
                {
                    if (std::string(entry.Name) == "test-ports")
                    {
                        testPortsId = entry.Id;
                        break;
                    }
                }
                VERIFY_IS_FALSE(testPortsId.empty());

                // Filter ports for this container
                std::vector<WSLCPortMapping> containerPorts;
                for (const auto& port : ports)
                {
                    if (testPortsId == port.Id)
                    {
                        containerPorts.push_back(port.PortMapping);
                    }
                }

                VERIFY_ARE_EQUAL(2, containerPorts.size());
                VERIFY_ARE_EQUAL(1234, containerPorts[0].HostPort);
                VERIFY_ARE_EQUAL(8000, containerPorts[0].ContainerPort);
                VERIFY_ARE_EQUAL(AF_INET, containerPorts[0].Family);
                VERIFY_ARE_EQUAL(1234, containerPorts[1].HostPort);
                VERIFY_ARE_EQUAL(8000, containerPorts[1].ContainerPort);
                VERIFY_ARE_EQUAL(AF_INET6, containerPorts[1].Family);
                VERIFY_ARE_EQUAL(IPPROTO_TCP, containerPorts[0].Protocol);
                VERIFY_ARE_EQUAL(IPPROTO_TCP, containerPorts[1].Protocol);
            }

            // Verify that a created (not yet started) container returns no ports.
            {
                WSLCContainerLauncher createdLauncher("debian:latest", "test-ports-created", {"echo", "OK"}, {}, containerNetworkType);
                createdLauncher.AddPort(1235, 8000, AF_INET);

                auto createdContainer = createdLauncher.Create(session);

                wsl::windows::common::wslc::unique_container_entry_array containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                std::string createdId = createdContainer.Id();
                for (const auto& port : ports)
                {
                    VERIFY_ARE_NOT_EQUAL(createdId, std::string(port.Id));
                }

                VERIFY_SUCCEEDED(createdContainer.Get().Delete(WSLCDeleteFlagsNone));
                createdContainer.Reset();
            }

            // Validate that the port cannot be reused while the container is running.
            WSLCContainerLauncher subLauncher(
                "python:3.12-alpine", "test-ports-2", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            subLauncher.AddPort(1234, 8000, AF_INET);

            auto [hresult, newContainer] = subLauncher.LaunchNoThrow(session);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(WSAEADDRINUSE));

            // Verify that a stopped container returns no ports.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            {
                wsl::windows::common::wslc::unique_container_entry_array containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                std::string stoppedId = container.Id();
                for (const auto& port : ports)
                {
                    VERIFY_ARE_NOT_EQUAL(stoppedId, std::string(port.Id));
                }
            }

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            container.Reset(); // TODO: Re-think container lifetime management.

            // Validate that the port can be reused now that the container is stopped.
            {
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-3", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

                launcher.AddPort(1234, 8000, AF_INET);

                auto container = launcher.Launch(session);
                auto initProcess = container.GetInitProcess();

                // Wait for the container bind() to be completed.
                WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on 0.0.0.0 port 8000");

                expectBoundPorts(container, {"8000/tcp"});
                ExpectHttpResponse(L"http://127.0.0.1:1234", 200);

                VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
                VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
                container.Reset(); // TODO: Re-think container lifetime management.
            }
        }

        // Validate that the same host port can't be bound twice in the same Create() call.
        {
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET);

            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));
        }

        auto bindSocket = [](auto port) {
            wil::unique_socket socket(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            VERIFY_ARE_NOT_EQUAL(bind(socket.get(), (sockaddr*)&address, sizeof(address)), SOCKET_ERROR);
            return socket;
        };

        // Validate that Create() fails if the port is already bound.
        {
            auto boundSocket = bindSocket(1235);
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1235, 8000, AF_INET);
            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));

            // Validate that Create() correctly cleans up bound ports after a port fails to map
            {
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);
                launcher.AddPort(1236, 8000, AF_INET); // Should succeed
                launcher.AddPort(1235, 8000, AF_INET); // Should fail.

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));

                // Validate that port 1236 is still available (was cleaned up after failure).
                VERIFY_IS_TRUE(!!bindSocket(1236));
            }
        }

        // Validate error paths
        {
            // Invalid IP address
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "invalid-ip");

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, E_INVALIDARG);
                ValidateCOMErrorMessage(L"Invalid IP address 'invalid-ip'");
            }

            // Invalid protocol
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, 1);

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, E_INVALIDARG);
            }

            // Invalid address family (launched manually because AddPort() throws on unsupported family).
            {
                WSLCPortMapping port{};
                strcpy_s(port.BindingAddress, "127.0.0.1");
                port.HostPort = 1234;
                port.ContainerPort = 1234;
                port.Protocol = IPPROTO_TCP;
                port.Family = AF_UNIX; // Unsupported

                WSLCContainerOptions options{};
                options.Image = "python:3.12-alpine";
                options.Ports = &port;
                options.PortsCount = 1;
                options.ContainerNetwork.NetworkMode = containerNetworkType.c_str();

                wil::com_ptr<IWSLCContainer> container;
                VERIFY_ARE_EQUAL(session.CreateContainer(&options, nullptr, &container), E_INVALIDARG);
            }

            if (virtionet)
            {
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_UDP);

                    VERIFY_SUCCEEDED(launcher.LaunchNoThrow(session).first);
                }

                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0");

                    VERIFY_SUCCEEDED(launcher.LaunchNoThrow(session).first);
                }
            }
            else
            {
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_UDP);

                    VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
                }

                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0");

                    VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
                }
            }
        }
    }

    auto SetupPortMappingsTest(WSLCNetworkingMode networkingMode, WSLCFeatureFlags featureFlags = WslcFeatureFlagsNone)
    {
        auto settings = GetDefaultSessionSettings(L"networking-session", true, networkingMode);
        settings.FeatureFlags = featureFlags;

        auto createNewSession = settings.NetworkingMode != m_defaultSessionSettings.NetworkingMode ||
                                settings.FeatureFlags != m_defaultSessionSettings.FeatureFlags;
        auto restore = createNewSession ? std::optional{ResetTestSession()} : std::nullopt;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        return std::make_pair(std::move(restore), std::move(session));
    }

    WSLC_TEST_METHOD(PortMappingsNat)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeNAT);

        RunPortMappingsTest(*session, "bridge", false);
        RunPortMappingsTest(*session, "host", false);
    }

    WSLC_TEST_METHOD(PortMappingsConsomme)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme);

        RunPortMappingsTest(*session, "bridge", true);
        RunPortMappingsTest(*session, "host", true);
    }

    WSLC_TEST_METHOD(PortMappingsConsommeWslRelay)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme, WslcFeatureFlagsPortRelayWslRelay);

        RunPortMappingsTest(*session, "bridge", false);
        RunPortMappingsTest(*session, "host", false);
    }

    WSLC_TEST_METHOD(PortMappingsAdvanced)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme);

        auto hostIp = GetHostAdapterIpv4();
        std::optional<std::string> hostIpNarrow;
        if (hostIp.has_value())
        {
            hostIpNarrow = wsl::shared::string::WideToMultiByte(hostIp.value());
        }

        struct PortMapping
        {
            uint16_t HostPort;
            uint16_t ContainerPort;
            int Family;
            int Protocol = IPPROTO_TCP;
            std::optional<std::string> BindingAddress;
        };

        auto runCustomBindingTests = [&](const std::string& containerNetworkType) {
            LogInfo("Container network type: %hs", containerNetworkType.c_str());

            auto createTcpContainer = [&](const std::vector<PortMapping>& ports) {
                static int containerIndex = 0;
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine",
                    std::format("test-ports-custom-{}", containerIndex++),
                    {"python3", "-m", "http.server", "--bind", "::"},
                    {"PYTHONUNBUFFERED=1"},
                    containerNetworkType);

                for (const auto& port : ports)
                {
                    launcher.AddPort(port.HostPort, port.ContainerPort, port.Family, port.Protocol, port.BindingAddress);
                }

                auto container = launcher.Launch(*session);
                WaitForOutput(container.GetInitProcess().GetStdHandle(1), "Serving HTTP on");
                return container;
            };

            auto validateInspectPortBinding = [&](auto& container,
                                                  uint16_t containerPort,
                                                  int protocol,
                                                  const std::string& expectedHostIp,
                                                  std::optional<uint16_t> expectedHostPort) -> std::string {
                auto inspectData = container.Inspect();

                auto portKey = std::format("{}/{}", containerPort, protocol == IPPROTO_UDP ? "udp" : "tcp");
                VERIFY_IS_TRUE(inspectData.Ports.contains(portKey));

                auto& bindings = inspectData.Ports[portKey];
                VERIFY_ARE_EQUAL(1u, bindings.size());
                VERIFY_ARE_EQUAL(expectedHostIp, bindings[0].HostIp);

                if (expectedHostPort.has_value())
                {
                    VERIFY_ARE_EQUAL(std::to_string(expectedHostPort.value()), bindings[0].HostPort);
                }
                else
                {
                    VERIFY_IS_TRUE(std::stoi(bindings[0].HostPort) > 0);
                }

                return bindings[0].HostPort;
            };

            // Explicit localhost (127.0.0.1) binding.
            {
                auto container = createTcpContainer({{1260, 8000, AF_INET, IPPROTO_TCP, "127.0.0.1"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "127.0.0.1", 1260);
                ExpectHttpResponse(L"http://127.0.0.1:1260", 200);
            }

            // 0.0.0.0 (all interfaces) binding.
            {
                auto container = createTcpContainer({{1261, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "0.0.0.0", 1261);

                // Verify reachable via loopback.
                ExpectHttpResponse(L"http://127.0.0.1:1261", 200);

                // Verify reachable via host adapter IP to confirm wildcard semantics.
                if (hostIp.has_value())
                {
                    auto url = std::format(L"http://{}:1261", hostIp.value());
                    ExpectHttpResponse(url.c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP verification: no suitable IPv4 adapter found");
                }
            }

            // Main host adapter's IPv4 address binding.
            {
                if (hostIp.has_value())
                {
                    auto container = createTcpContainer({{1262, 8000, AF_INET, IPPROTO_TCP, hostIpNarrow.value()}});
                    validateInspectPortBinding(container, 8000, IPPROTO_TCP, hostIpNarrow.value(), 1262);

                    auto url = std::format(L"http://{}:1262", hostIp.value());
                    ExpectHttpResponse(url.c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP binding test: no suitable IPv4 adapter found");
                }
            }

            // Anonymous bind on localhost (ephemeral host port).
            {
                auto container = createTcpContainer({{WSLC_EPHEMERAL_PORT, 8000, AF_INET, IPPROTO_TCP, "127.0.0.1"}});
                auto hostPort = validateInspectPortBinding(container, 8000, IPPROTO_TCP, "127.0.0.1", std::nullopt);

                ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", hostPort).c_str(), 200);
            }

            // Anonymous bind on host ip (ephemeral host port).
            {
                if (hostIp.has_value())
                {
                    auto container = createTcpContainer({{WSLC_EPHEMERAL_PORT, 8000, AF_INET, IPPROTO_TCP, hostIpNarrow.value()}});
                    auto hostPort = validateInspectPortBinding(container, 8000, IPPROTO_TCP, hostIpNarrow.value(), std::nullopt);

                    ExpectHttpResponse(std::format(L"http://{}:{}", hostIp.value(), hostPort).c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP binding test: no suitable IPv4 adapter found");
                }
            }

            // IPv6 loopback (::1) binding.
            {
                auto container = createTcpContainer({{1263, 8000, AF_INET6, IPPROTO_TCP, "::1"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "::1", 1263);
                ExpectHttpResponse(L"http://[::1]:1263", 200);
            }

            // IPv6 wildcard (::) binding.
            {
                auto container = createTcpContainer({{1264, 8000, AF_INET6, IPPROTO_TCP, "::"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "::", 1264);
                ExpectHttpResponse(L"http://[::1]:1264", 200);
            }

            // UDP port mapping with a Python echo server.
            {
                // Inline Python UDP echo server: receives a datagram and sends it back uppercased.
                static constexpr auto c_udpEchoScript =
                    "import socket,sys;"
                    "s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM);"
                    "s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,0);"
                    "s.bind(('::',9000));"
                    "print('UDP listening',flush=True);"
                    "data,addr=s.recvfrom(1024);"
                    "s.sendto(data.upper(),addr)";

                static int udpContainerIndex = 0;
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine",
                    std::format("test-ports-custom-udp-{}", udpContainerIndex++),
                    {"python3", "-c", c_udpEchoScript},
                    {"PYTHONUNBUFFERED=1"},
                    containerNetworkType);

                launcher.AddPort(1265, 9000, AF_INET, IPPROTO_UDP, "127.0.0.1");

                auto container = launcher.Launch(*session);
                WaitForOutput(container.GetInitProcess().GetStdHandle(1), "UDP listening");
                validateInspectPortBinding(container, 9000, IPPROTO_UDP, "127.0.0.1", 1265);

                WSLCE2ETests::SendUdpAndReceive(1265, "hello", "HELLO");
            }

            // Validate that trying to bind an address that the host doesn't have fails:
            {
                // Malformed address string.
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1265, 8000, AF_INET, IPPROTO_TCP, "1.1.1.1");

                    auto container = launcher.Create(*session);
                    validateInspectPortBinding(container, 8000, IPPROTO_TCP, "1.1.1.1", 1265);

                    VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), HRESULT_FROM_WIN32(WSAEADDRNOTAVAIL));
                    ValidateCOMErrorMessage(
                        L"Failed to map port '1.1.1.1:1265/tcp', The requested address is not valid in its context. ");
                }
            }
        };

        runCustomBindingTests("bridge");
        runCustomBindingTests("host");
    }

    TEST_METHOD(PortMappingsNone)
    {
        // Validate that trying to map ports without network fails.
        WSLCContainerLauncher launcher(
            "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, "none");

        launcher.AddPort(1234, 8000, AF_INET);

        VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*m_defaultSession).first, E_INVALIDARG);
    }

    WSLC_TEST_METHOD(PublishAllExposedPorts)
    {
        // Build a test image with EXPOSE directives.
        auto contextDir = std::filesystem::current_path() / "build-context-publish-all";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-publish-all:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        // TODO: Add test coverage for exposed UDP ports once supported.
        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM python:3.12-alpine\n";
            dockerfile << "EXPOSE 8080/tcp\n";
            dockerfile << "EXPOSE 9090/tcp\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-publish-all:latest"));

        // Run a container with --publish-all using the API.
        {
            WSLCContainerLauncher launcher(
                "wslc-test-publish-all:latest",
                "test-publish-all",
                {"python3", "-m", "http.server", "--bind", "::", "8080"},
                {"PYTHONUNBUFFERED=1"},
                "bridge");

            launcher.SetContainerFlags(WSLCContainerFlagsPublishAll);

            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

            // Verify the container has port mappings for the exposed ports.
            auto inspectData = container.Inspect();
            VERIFY_IS_TRUE(inspectData.Ports.contains("8080/tcp"));
            VERIFY_IS_TRUE(inspectData.Ports.contains("9090/tcp"));

            // Each exposed port is published dual-stack: one IPv4 (127.0.0.1) and one IPv6 (::1)
            // loopback binding. Verify both are present and return both host ports.
            struct DualStackHostPorts
            {
                int ipv4 = 0;
                int ipv6 = 0;
            };

            auto getDualStackHostPorts = [](const auto& bindings) -> DualStackHostPorts {
                VERIFY_ARE_EQUAL(2u, bindings.size());

                DualStackHostPorts ports;
                for (const auto& binding : bindings)
                {
                    auto hostPort = std::stoi(binding.HostPort);
                    VERIFY_IS_TRUE(hostPort > 0);
                    if (binding.HostIp == "127.0.0.1")
                    {
                        ports.ipv4 = hostPort;
                    }
                    else if (binding.HostIp == "::1")
                    {
                        ports.ipv6 = hostPort;
                    }
                }

                VERIFY_IS_TRUE(ports.ipv4 > 0);
                VERIFY_IS_TRUE(ports.ipv6 > 0);
                return ports;
            };

            // Verify we can reach the 8080 exposed port over both IPv4 and IPv6 loopback.
            auto ports8080 = getDualStackHostPorts(inspectData.Ports["8080/tcp"]);
            ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", ports8080.ipv4).c_str(), 200);
            ExpectHttpResponse(std::format(L"http://[::1]:{}", ports8080.ipv6).c_str(), 200);

            // Verify the second exposed port got a dual-stack mapping too.
            auto ports9090 = getDualStackHostPorts(inspectData.Ports["9090/tcp"]);

            // Each exposed port must map to distinct host ports on both loopback families.
            VERIFY_ARE_NOT_EQUAL(ports8080.ipv4, ports9090.ipv4);
            VERIFY_ARE_NOT_EQUAL(ports8080.ipv6, ports9090.ipv6);
        }
    }

    WSLC_TEST_METHOD(PublishAllImageNotFound)
    {
        // Verify that using PublishAll with a nonexistent image still returns IMAGE_NOT_FOUND.
        WSLCContainerLauncher launcher("invalid-image-name:nonexistent", "dummy-publish-all", {"/bin/cat"}, {}, "bridge");
        launcher.SetContainerFlags(WSLCContainerFlagsPublishAll);

        auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(hresult, WSLC_E_IMAGE_NOT_FOUND);
    }
};
