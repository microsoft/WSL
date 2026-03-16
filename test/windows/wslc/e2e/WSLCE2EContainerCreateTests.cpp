/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerCreateTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wil/network.h>
#include <wil/resource.h>

namespace WSLCE2ETests {

class WSLCE2EContainerCreateTests
{
    WSL_TEST_CLASS(WSLCE2EContainerCreateTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);

        // Initialize Winsock for loopback connectivity tests
        WSADATA wsaData{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        THROW_HR_IF(HRESULT_FROM_WIN32(result), result != 0);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);

        // Cleanup Winsock
        WSACleanup();
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Create_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --name " + WslcContainerName);
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + InvalidImage.NameAndTag());
        std::wstringstream expectedError;
        expectedError << L"Image '" << InvalidImage.NameAndTag() << L"' not found, pulling\r\n"
                      << L"pull access denied for library/"
                      << InvalidImage.Name << L", repository does not exist or may require 'docker login': denied: requested access to the resource is denied\r\n"
                      << L"Error code: WSLA_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");
    }

    TEST_METHOD(WSLCE2E_Container_Create_DuplicateContainerName)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        // Attempt to create another container with the same name
        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = std::format(L"Conflict. The container name \"/{}\" is already in use by container \"{}\". You have to remove (or rename) that container to be able to reuse that name.\r\nError code: ERROR_ALREADY_EXISTS\r\n", WslcContainerName, containerId),
             .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Remove)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Start the container.
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify with retry timeout of 1 minute.
        VerifyContainerIsNotListed(WslcContainerName, std::chrono::seconds(2), std::chrono::minutes(1));
    }

    TEST_METHOD(WSLCE2E_Container_Run_Remove)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Run the container with a valid image
        auto result = RunWslc(std::format(L"container run --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Run should be deleted on return so no retry.
        VerifyContainerIsNotListed(WslcContainerName);
    }

    TEST_METHOD(WSLCE2E_Container_Run_Port_TCP)
    {
        WSL2_TEST_ONLY();

        // Start a container with a simple server listening on a port
        auto hostPort = GetFreePort();
        auto message = L"WSLC E2E Test";
        auto result = RunWslc(
            std::format(
                L"container run -d --name {} -p {}:{} {} perl -e \"{}\"",
                WslcContainerName,
                hostPort,
                ContainerTestPort,
                DebianImage.NameAndTag(),
                GetPerlHttpServerCode(message, ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // From the host side, verify we can connect to the server and receive
        // the expected message
        auto response = ReadFromLoopback(hostPort);
        VERIFY_ARE_EQUAL(message, response);

        // Also verify the port mapping is correct in the container inspect data
        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(hostPort), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", portBindings[0].HostIp);
    }

    TEST_METHOD(WSLCE2E_Container_Run_PortMultipleMappings)
    {
        WSL2_TEST_ONLY();

        // Start a container with a simple server listening on a port
        // Map two host ports to the same container port
        auto hostPort1 = GetFreePort();
        auto hostPort2 = GetFreePort();
        VERIFY_ARE_NOT_EQUAL(hostPort1, hostPort2);

        auto message = L"WSLC E2E Test";
        auto result = RunWslc(
            std::format(
                L"container run -d --name {} -p {}:{} -p {}:{} {} perl -e \"{}\"",
                WslcContainerName,
                hostPort1,
                ContainerTestPort,
                hostPort2,
                ContainerTestPort,
                DebianImage.NameAndTag(),
                GetPerlHttpServerCode(message, ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // From the host side, verify we can connect to both ports and receive
        // the expected message
        auto response8090 = ReadFromLoopback(hostPort1);
        VERIFY_ARE_EQUAL(message, response8090);

        auto response8091 = ReadFromLoopback(hostPort2);
        VERIFY_ARE_EQUAL(message, response8091);
    }

    TEST_METHOD(WSLCE2E_Container_Run_PortAlreadyInUse)
    {
        // Bug: https://github.com/microsoft/WSL/issues/14448
        SKIP_TEST_NOT_IMPL();

        WSL2_TEST_ONLY();

        // Start a container with a simple server listening on a port
        auto hostPort = GetFreePort();
        auto message = L"WSLC E2E Test";
        auto result1 = RunWslc(
            std::format(
                L"container run -d --name {} -p {}:{} {} perl -e \"{}\"",
                WslcContainerName,
                hostPort,
                ContainerTestPort,
                DebianImage.NameAndTag(),
                GetPerlHttpServerCode(message, ContainerTestPort)));
        result1.Verify({.Stderr = L"", .ExitCode = 0});

        // Attempt to start another container mapping the same host port
        auto result2 = RunWslc(std::format(L"container run -p {}:{} {}", hostPort, ContainerTestPort, DebianImage.NameAndTag()));
        result2.Verify({.ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    TEST_METHOD(WSLCE2E_Container_Run_PortEphemeral_NotSupported)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -p 80 {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    TEST_METHOD(WSLCE2E_Container_Run_PortUdp_NotSupported)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -p 80:80/udp {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    TEST_METHOD(WSLCE2E_Container_Run_PortHostIP_NotSupported)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -p 127.0.0.1:80:80 {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const uint16_t ContainerTestPort = 8080;

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Creates a container.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container create [<options>] <image> [<command>] [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  image             Image name\r\n"
                 << L"  command           The command to run\r\n"
                 << L"  arguments         Arguments to pass to container's init process\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  --cidfile         Write the container ID to the provided path.\r\n"
                << L"  --dns             IP address of the DNS nameserver in resolv.conf\r\n"
                << L"  --dns-domain      Set the default DNS Domain\r\n"
                << L"  --dns-option      Set DNS options\r\n"
                << L"  --dns-search      Set DNS search domains\r\n"
                << L"  --entrypoint      Specifies the container init process executable\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  --groupid         Group Id for the process\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --name            Name of the container\r\n"
                << L"  --no-dns          No configuration of DNS in the container\r\n"
                << L"  --progress        Progress type (format: none|ansi) (default: ansi)\r\n"
                << L"  -p,--publish      Publish a port from a container to host\r\n"
                << L"  --rm              Remove the container after it stops\r\n"
                << L"  --scheme          Use this scheme for registry connection\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  --tmpfs           Mount tmpfs to the container at the given path\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  --volume          Bind mount a volume to the container\r\n"
                << L"  --virtualization  Expose virtualization capabilities to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n\r\n";
        return options.str();
    }

    std::wstring ReadFromLoopback(uint16_t port)
    {
        SOCKADDR_IN address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        auto readOnce = [&]() -> std::wstring {
            std::string response;

            wil::unique_socket clientSocket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            THROW_HR_IF(E_FAIL, clientSocket.get() == INVALID_SOCKET);
            THROW_HR_IF(E_FAIL, ::connect(clientSocket.get(), reinterpret_cast<SOCKADDR*>(&address), sizeof(address)) == SOCKET_ERROR);

            char buffer[256]{};
            for (;;)
            {
                const int bytesRead = ::recv(clientSocket.get(), buffer, sizeof(buffer), 0);
                THROW_HR_IF(E_FAIL, bytesRead == SOCKET_ERROR);

                if (bytesRead == 0)
                {
                    break;
                }

                response.append(buffer, bytesRead);
            }

            // Empty payload is treated as transient startup/routing failure.
            THROW_HR_IF(E_FAIL, response.empty());
            return wsl::shared::string::MultiByteToWide(response);
        };

        return wsl::shared::retry::RetryWithTimeout<std::wstring>(readOnce, std::chrono::milliseconds(500), std::chrono::minutes(1), []() {
            return wil::ResultFromCaughtException() == E_FAIL;
        });
    }

    std::wstring GetPerlHttpServerCode(std::wstring message, uint16_t port)
    {
        std::wstringstream ss;
        ss << L"use IO::Socket::INET;"            //
           << L"my $s = IO::Socket::INET->new("   //
           << L"  LocalPort => " << port << L","  //
           << L"  Listen    => 5,"                //
           << L"  Reuse     => 1"                 //
           << L") or die $!;"                     //
           << L"while (my $c = $s->accept) {"     //
           << L"  print $c '" << message << L"';" //
           << L"  close $c;"                      //
           << L"}";                               //
        return ss.str();
    }

    uint16_t GetFreePort()
    {
        // Note: This function finds a free port by binding to port 0, which
        // tells the OS to assign an available port. While there is a potential
        // race condition where the port could be taken by another process
        // before it is used, this is acceptable for testing purposes.

        wil::unique_socket sock(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        THROW_LAST_ERROR_IF(sock.get() == INVALID_SOCKET);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        THROW_LAST_ERROR_IF(bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);

        sockaddr_in assigned{};
        int len = sizeof(assigned);

        THROW_LAST_ERROR_IF(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&assigned), &len) == SOCKET_ERROR);
        return ntohs(assigned.sin_port);
    }
};
} // namespace WSLCE2ETests