/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLATests.cpp

Abstract:

    This file contains test cases for the WSLA API.

--*/

#include "precomp.h"
#include "Common.h"
#include "WSLAApi.h"
#include "wslaservice.h"
#include "WSLAProcessLauncher.h"
#include "WslCoreFilesystem.h"

using namespace wsl::windows::common::registry;
using wsl::windows::common::ProcessFlags;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;

class WSLATests
{
    WSL_TEST_CLASS(WSLATests)
    wil::unique_couninitialize_call coinit = wil::CoInitializeEx();
    WSADATA Data;
    std::filesystem::path testVhd;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

        auto distroKey = OpenDistributionKey(LXSS_DISTRO_NAME_TEST_L);

        auto vhdPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath");
        testVhd = std::filesystem::path{vhdPath} / "ext4.vhdx";

        WslShutdown();
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    wil::com_ptr<IWSLASession> CreateSession(VIRTUAL_MACHINE_SETTINGS& vmSettings, const WSLA_SESSION_SETTINGS& sessionSettings = {L"wsla-test"})
    {
        vmSettings.RootVhdType = "ext4";

        wil::com_ptr<IWSLAUserSession> userSession;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        wil::com_ptr<IWSLASession> session;

        VERIFY_SUCCEEDED(userSession->CreateSession(&sessionSettings, &vmSettings, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        return session;
    }

    TEST_METHOD(GetVersion)
    {
        wil::com_ptr<IWSLAUserSession> userSession;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));

        WSLA_VERSION version{};

        VERIFY_SUCCEEDED(userSession->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    RunningWSLAProcess::ProcessResult RunCommand(IWSLASession* session, const std::vector<std::string>& command, int timeout = 600000)
    {
        WSLAProcessLauncher process(command[0], command);

        return process.Launch(*session).WaitAndCaptureOutput();
    }

    RunningWSLAProcess::ProcessResult ExpectCommandResult(
        IWSLASession* session, const std::vector<std::string>& command, int expectResult, bool expectSignal = false, int timeout = 600000)
    {
        auto result = RunCommand(session, command, timeout);

        if (result.Signalled != expectSignal)
        {
            auto cmd = wsl::shared::string::Join(command, ' ');

            if (expectSignal)
            {
                LogError(
                    "Command: %hs didn't get signalled as expected. ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                    cmd.c_str(),
                    result.Code,
                    result.Output[1].c_str(),
                    result.Output[2].c_str());
            }
            else
            {
                LogError(
                    "Command: %hs didn't receive an unexpected signal: %i. Stdout: '%hs', Stderr: '%hs'",
                    cmd.c_str(),
                    result.Code,
                    result.Output[1].c_str(),
                    result.Output[2].c_str());
            }
        }

        if (result.Code != expectResult)
        {
            auto cmd = wsl::shared::string::Join(command, ' ');
            LogError(
                "Command: %hs didn't return expected code (%i). ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                cmd.c_str(),
                expectResult,
                result.Code,
                result.Output[1].c_str(),
                result.Output[2].c_str());
        }

        return result;
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            VIRTUAL_MACHINE_SETTINGS settings{};
            settings.CpuCount = 4;
            settings.DisplayName = L"WSLA";
            settings.MemoryMb = 2048;
            settings.BootTimeoutMs = 30 * 1000;
            settings.DmesgOutput = (ULONG) reinterpret_cast<ULONG_PTR>(write.get());
            settings.EnableEarlyBootDmesg = earlyBootLogging;
            settings.RootVhd = testVhd.c_str();

            std::vector<char> dmesgContent;
            auto readDmesg = [read = read.get(), &dmesgContent]() mutable {
                DWORD Offset = 0;

                constexpr auto bufferSize = 1024;
                while (true)
                {
                    dmesgContent.resize(Offset + bufferSize);

                    DWORD Read{};
                    if (!ReadFile(read, &dmesgContent[Offset], bufferSize, &Read, nullptr))
                    {
                        LogInfo("ReadFile() failed: %lu", GetLastError());
                    }

                    if (Read == 0)
                    {
                        break;
                    }

                    Offset += Read;
                }
            };

            std::thread thread(readDmesg); // Needs to be created before the VM starts, to avoid a pipe deadlock.

            auto session = CreateSession(settings);
            auto detach = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                session->Shutdown(30 * 1000);
                if (thread.joinable())
                {
                    thread.join();
                }
            });

            write.reset();

            ExpectCommandResult(session.get(), {"/bin/bash", "-c", "echo DmesgTest > /dev/kmsg"}, 0);

            VERIFY_ARE_EQUAL(session->Shutdown(30 * 1000), S_OK);
            detach.reset();

            auto contentString = std::string(dmesgContent.begin(), dmesgContent.end());

            VERIFY_ARE_NOT_EQUAL(contentString.find("Run /init as init process"), std::string::npos);
            VERIFY_ARE_NOT_EQUAL(contentString.find("DmesgTest"), std::string::npos);

            return contentString;
        };

        auto validateFirstDmesgLine = [](const std::string& dmesg, const char* expected) {
            auto firstLf = dmesg.find("\n");
            VERIFY_ARE_NOT_EQUAL(firstLf, std::string::npos);
            VERIFY_IS_TRUE(dmesg.find(expected) < firstLf);
        };

        // Dmesg without early boot logging
        {
            auto dmesg = createVmWithDmesg(false);

            // Verify that the first line is "brd: module loaded";
            validateFirstDmesgLine(dmesg, "brd: module loaded");
        }

        // Dmesg with early boot logging
        {
            auto dmesg = createVmWithDmesg(true);
            validateFirstDmesgLine(dmesg, "Linux version");
        }
    }

    TEST_METHOD(TerminationCallback)
    {
        WSL2_TEST_ONLY();

        class DECLSPEC_UUID("7BC4E198-6531-4FA6-ADE2-5EF3D2A04DFF") CallbackInstance
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback, IFastRundown>
        {

        public:
            CallbackInstance(std::function<void(WSLAVirtualMachineTerminationReason, LPCWSTR)>&& callback) :
                m_callback(std::move(callback))
            {
            }

            HRESULT OnTermination(WSLAVirtualMachineTerminationReason Reason, LPCWSTR Details) override
            {
                m_callback(Reason, Details);
                return S_OK;
            }

        private:
            std::function<void(WSLAVirtualMachineTerminationReason, LPCWSTR)> m_callback;
        };

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        std::promise<std::pair<WSLAVirtualMachineTerminationReason, std::wstring>> promise;

        CallbackInstance callback{[&](WSLAVirtualMachineTerminationReason reason, LPCWSTR details) {
            promise.set_value(std::make_pair(reason, details));
        }};

        WSLA_SESSION_SETTINGS sessionSettings{L"wsla-test"};
        sessionSettings.TerminationCallback = &callback;

        auto session = CreateSession(settings, sessionSettings);

        wil::com_ptr<IWSLAVirtualMachine> vm;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));
        VERIFY_SUCCEEDED(vm->Shutdown(30 * 1000));
        auto future = promise.get_future();
        auto result = future.wait_for(std::chrono::seconds(30));
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, WSLAVirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        WSLAProcessLauncher launcher("/bin/sh", {"/bin/sh"}, {"TERM=xterm-256color"}, ProcessFlags::None);
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});

        auto process = launcher.Launch(*session);

        wil::unique_handle ttyInput = process.GetStdHandle(0);
        wil::unique_handle ttyOutput = process.GetStdHandle(1);

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(ttyOutput.get(), buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(ttyInput.get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("#");
        writeTty("echo OK\n");
        validateTtyOutput(" echo OK\r\nOK");

        // Exit the shell
        writeTty("exit\n");

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 1000));
    }

    TEST_METHOD(NATNetworking)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = WSLANetworkingModeNAT;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/bash",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);
    }

    TEST_METHOD(NATNetworkingWithDnsTunneling)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = WSLANetworkingModeNAT;
        settings.EnableDnsTunneling = true;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/bash",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        // Verify that /etc/resolv.conf is correctly configured.
        auto result = ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver ", "/etc/resolv.conf"}, 0);

        VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
    }

    TEST_METHOD(VirtioProxyNetworking)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = WSLANetworkingModeVirtioProxy;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/bash",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);
    }

    TEST_METHOD(OpenFiles)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        struct FileFd
        {
            int Fd;
            WSLAFdType Flags;
            const char* Path;
        };

        auto createProcess = [&](const std::vector<std::string>& Args, const std::vector<FileFd>& Fds, HRESULT expectedError = S_OK) {
            WSLAProcessLauncher launcher(Args[0], Args, {}, ProcessFlags::None);

            for (const auto& e : Fds)
            {
                launcher.AddFd(WSLA_PROCESS_FD{.Fd = e.Fd, .Type = e.Flags, .Path = e.Path});
            }

            auto [hresult, _, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, expectedError);

            return process;
        };

        {
            auto process =
                createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WSLAFdTypeDefault, nullptr}});

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Output[1], "cat\n");
        }

        {

            auto read = [&]() {
                auto process =
                    createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileInput, "/tmp/output"}, {1, WSLAFdTypeDefault, nullptr}});
                return process->WaitAndCaptureOutput().Output[1];
            };

            // Write to a new file.
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr},
                 {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput | WSLAFdTypeLinuxFileCreate), "/tmp/output"}});

            constexpr auto content = "TestOutput";
            VERIFY_IS_TRUE(WriteFile(process->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);

            // Append content to the same file
            auto appendProcess = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr},
                 {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput | WSLAFdTypeLinuxFileAppend), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(appendProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(appendProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), std::format("{}{}", content, content));

            // Truncate the file
            auto truncProcess = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr}, {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(truncProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(truncProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);
        }

        // Test various error paths
        {
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), "/tmp/DoesNotExist"}}, E_FAIL);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeDefault), "should-be-null"}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeDefault | WSLAFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileInput | WSLAFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
        }

        // Validate that read & write modes are respected
        {
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WSLAFdTypeLinuxFileInput, "/tmp/output"}, {2, WSLAFdTypeDefault, nullptr}});

            auto result = process->WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[2], "/bin/cat: write error: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(result.Code, 1);
        }

        {
            auto process = createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileOutput, "/tmp/output"}, {2, WSLAFdTypeDefault, nullptr}});
            auto result = process->WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(result.Output[2], "/bin/cat: standard output: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(result.Code, 1);
        }
    }

    TEST_METHOD(NATPortMapping)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = WSLANetworkingModeNAT;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        wil::com_ptr<IWSLAVirtualMachine> vm;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));

        auto waitForOutput = [](HANDLE Handle, const char* Content) {
            std::string output;
            DWORD index = 0;
            while (true) // TODO: timeout
            {
                constexpr auto bufferSize = 100;

                output.resize(output.size() + bufferSize);
                DWORD bytesRead = 0;
                if (!ReadFile(Handle, &output[index], bufferSize, &bytesRead, nullptr))
                {
                    LogError("ReadFile failed with %lu", GetLastError());
                    VERIFY_FAIL();
                }

                output.resize(index + bytesRead);

                if (bytesRead == 0)
                {
                    LogError("Process exited, output: %hs", output.c_str());
                    VERIFY_FAIL();
                }

                index += bytesRead;
                if (output.find(Content) != std::string::npos)
                {
                    break;
                }
            }
        };

        auto listen = [&](short port, const char* content, bool ipv6) {
            auto cmd = std::format("echo -n '{}' | /usr/bin/socat -dd TCP{}-LISTEN:{},reuseaddr -", content, ipv6 ? "6" : "", port);
            auto process = WSLAProcessLauncher("/bin/bash", {"/bin/bash", "-c", cmd}).Launch(*session);
            waitForOutput(process.GetStdHandle(2).get(), "listening on");

            return process;
        };

        auto connectAndRead = [&](short port, int family) -> std::string {
            SOCKADDR_INET addr{};
            addr.si_family = family;
            INETADDR_SETLOOPBACK((PSOCKADDR)&addr);
            SS_PORT(&addr) = htons(port);

            wil::unique_socket hostSocket{socket(family, SOCK_STREAM, IPPROTO_TCP)};
            THROW_LAST_ERROR_IF(!hostSocket);
            THROW_LAST_ERROR_IF(connect(hostSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);

            return ReadToString(hostSocket.get());
        };

        auto expectContent = [&](short port, int family, const char* expected) {
            auto content = connectAndRead(port, family);
            VERIFY_ARE_EQUAL(content, expected);
        };

        auto expectNotBound = [&](short port, int family) {
            auto result = wil::ResultFromException([&]() { connectAndRead(port, family); });

            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(WSAECONNREFUSED));
        };

        // Map port
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET, 1234, 80, false));

        // Validate that the same port can't be bound twice
        VERIFY_ARE_EQUAL(vm->MapPort(AF_INET, 1234, 80, false), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Check simple case
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that same port mapping can be reused
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that the connection is immediately reset if the port is not bound on the linux side
        expectContent(1234, AF_INET, "");

        // Add a ipv6 binding
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET6, 1234, 80, false));

        // Validate that ipv6 bindings work as well.
        listen(80, "port80ipv6", true);
        expectContent(1234, AF_INET6, "port80ipv6");

        // Unmap the ipv4 port
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET, 1234, 80, true));

        // Verify that a proper error is returned if the mapping doesn't exist
        VERIFY_ARE_EQUAL(vm->MapPort(AF_INET, 1234, 80, true), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        // Unmap the v6 port
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET6, 1234, 80, true));

        // Map another port as v6 only
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET6, 1235, 81, false));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(vm->MapPort(AF_INET6, 1235, 81, true));
        VERIFY_ARE_EQUAL(vm->MapPort(AF_INET6, 1235, 81, true), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(vm->MapPort(AF_INET, 1234, 80, false));

        auto process =
            WSLAProcessLauncher{"/usr/bin/socat", {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"}}
                .Launch(*session);

        waitForOutput(process.GetStdHandle(2).get(), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(vm->MapPort(AF_INET, 1234, 80, true));
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        // Create a 'stuck' process
        auto process = WSLAProcessLauncher{"/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout}.Launch(*session);

        // Stop the service
        StopWslaService();
    }

    TEST_METHOD(WindowsMounts)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        wil::com_ptr<IWSLAVirtualMachine> vm;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));
        wsl::windows::common::security::ConfigureForCOMImpersonation(vm.get());

        auto expectMount = [&](const std::string& target, const std::optional<std::string>& options) {
            auto cmd = std::format("set -o pipefail ; findmnt '{}' | tail  -n 1", target);

            auto result = ExpectCommandResult(session.get(), {"/bin/bash", "-c", cmd}, options.has_value() ? 0 : 1);

            const auto& output = result.Output[1];
            const auto& error = result.Output[2];

            if (result.Code != (options.has_value() ? 0 : 1))
            {
                LogError("%hs failed. code=%i, output: %hs, error: %hs", cmd.c_str(), result.Code, output.c_str(), error.c_str());
                VERIFY_FAIL();
            }

            if (options.has_value() && !PathMatchSpecA(output.c_str(), options->c_str()))
            {
                std::wstring message = std::format(L"Output: '{}' didn't match pattern: '{}'", output, options.value());
                VERIFY_FAIL(message.c_str());
            }
        };

        auto testFolder = std::filesystem::current_path() / "test-folder";
        std::filesystem::create_directories(testFolder);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        // Validate writeable mount.
        {
            VERIFY_SUCCEEDED(vm->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            // Validate that mount can't be stacked on each other
            VERIFY_ARE_EQUAL(vm->MountWindowsFolder(testFolder.c_str(), "/win-path", false), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that folder is writeable from linux
            ExpectCommandResult(session.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt && sync"}, 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(vm->UnmountWindowsFolder("/win-path"));
            expectMount("/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(vm->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            // Validate that folder is not writeable from linux
            ExpectCommandResult(session.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(vm->UnmountWindowsFolder("/win-path"));
            expectMount("/win-path", {});
        }

        // Validate various error paths
        {
            VERIFY_ARE_EQUAL(vm->MountWindowsFolder(L"relative-path", "/win-path", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(vm->MountWindowsFolder(L"C:\\does-not-exist", "/win-path", true), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            VERIFY_ARE_EQUAL(vm->UnmountWindowsFolder("/not-mounted"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(vm->UnmountWindowsFolder("/proc"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(vm->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(vm->UnmountWindowsFolder("/win-path"));
        }
    }

    // This test case validates that no file descriptors are leaked to user processes.
    TEST_METHOD(Fd)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);
        auto result = ExpectCommandResult(
            session.get(), {"/bin/bash", "-c", "echo /proc/self/fd/* && (readlink -v /proc/self/fd/* || true)"}, 0);

        // Note: fd/0 is opened by readlink to read the actual content of /proc/self/fd.
        if (!PathMatchSpecA(result.Output[1].c_str(), "/proc/self/fd/0 /proc/self/fd/1 /proc/self/fd/2\nsocket:[*]\nsocket:[*]\n"))
        {
            LogInfo("Found additional fds: %hs", result.Output[1].c_str());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(GPU)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.EnableGPU = true;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        wil::com_ptr<IWSLAVirtualMachine> vm;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));

        // Validate that the GPU device is available.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "test -c /dev/dxg"}, 0);
        auto expectMount = [&](const std::string& target, const std::optional<std::string>& options) {
            auto cmd = std::format("set -o pipefail ; findmnt '{}' | tail  -n 1", target);
            WSLAProcessLauncher launcher{"/bin/bash", {"/bin/bash", "-c", cmd}};

            auto result = launcher.Launch(*session).WaitAndCaptureOutput();
            const auto& output = result.Output[1];
            const auto& error = result.Output[2];
            if (result.Code != (options.has_value() ? 0 : 1))
            {
                LogError("%hs failed. code=%i, output: %hs, error: %hs", cmd.c_str(), result.Code, output.c_str(), error.c_str());
                VERIFY_FAIL();
            }

            if (options.has_value() && !PathMatchSpecA(output.c_str(), options->c_str()))
            {
                std::wstring message = std::format(L"Output: '{}' didn't match pattern: '{}'", output, options.value());
                VERIFY_FAIL(message.c_str());
            }
        };

        expectMount(
            "/usr/lib/wsl/drivers",
            "/usr/lib/wsl/drivers*9p*relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");
        expectMount("/usr/lib/wsl/lib", "/usr/lib/wsl/lib none*overlay ro,relatime,lowerdir=/usr/lib/wsl/lib/packaged*");

        // Validate that the mount points are not writeable.
        VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}).Code, 1L);
        VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}).Code, 1L);

        // Validate that trying to mount the shares without GPU support disabled fails.
        {
            settings.EnableGPU = false;
            session = CreateSession(settings);

            wil::com_ptr<IWSLAVirtualMachine> vm;
            VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));

            // Validate that the GPU device is not available.
            expectMount("/usr/lib/wsl/drivers", {});
            expectMount("/usr/lib/wsl/lib", {});
        }
    }

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        // Use the system distro vhd for modprobe & lsmod.

#ifdef WSL_SYSTEM_DISTRO_PATH

        auto rootfs = std::filesystem::path(TEXT(WSL_SYSTEM_DISTRO_PATH));

#else
        auto rootfs = std::filesystem::path(wsl::windows::common::wslutil::GetMsiPackagePath().value()) / L"system.vhd";

#endif
        settings.RootVhd = rootfs.c_str();

        auto session = CreateSession(settings);

        // Sanity check.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(session.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    TEST_METHOD(CreateRootNamespaceProcess)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        // Simple case
        {
            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo OK"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Output[2], "");
        }

        // Stdout + stderr
        {

            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo stdout && (echo stderr 1>& 2)"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "stdout\n");
            VERIFY_ARE_EQUAL(result.Output[2], "stderr\n");
        }

        // Write a large stdin buffer and expect it back on stdout.
        {
            std::vector<char> largeBuffer;
            std::string pattern = "ExpectedBufferContent";

            for (size_t i = 0; i < 1024 * 1024; i++)
            {
                largeBuffer.insert(largeBuffer.end(), pattern.begin(), pattern.end());
            }

            WSLAProcessLauncher launcher(
                "/bin/sh", {"/bin/sh", "-c", "cat && (echo completed 1>& 2)"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(process.GetStdHandle(0), largeBuffer));
            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_IS_TRUE(std::equal(largeBuffer.begin(), largeBuffer.end(), result.Output[1].begin(), result.Output[1].end()));
            VERIFY_ARE_EQUAL(result.Output[2], "completed\n");
        }

        // Create a stuck process and kill it.
        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            // Try to send invalid signal to the process
            VERIFY_ARE_EQUAL(process.Get().Signal(9999), E_FAIL);

            // Send SIGKILL(9) to the process.
            VERIFY_SUCCEEDED(process.Get().Signal(9));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 9);
            VERIFY_ARE_EQUAL(result.Signalled, true);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            // Validate that process can't be signalled after it exited.
            VERIFY_ARE_EQUAL(process.Get().Signal(9), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Validate that errno is correctly propagated
        {
            WSLAProcessLauncher launcher("doesnotexist", {});

            auto [hresult, error, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 2); // ENOENT
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/", {});

            auto [hresult, error, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 13); // EACCESS
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);
            auto dummyHandle = process.GetStdHandle(1);

            // Verify that the same handle can only be acquired once.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(1, reinterpret_cast<ULONG*>(&dummyHandle)), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that trying to acquire a std handle that doesn't exist fails as expected.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(3, reinterpret_cast<ULONG*>(&dummyHandle)), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that the process object correctly handle requests after the VM has terminated.
            VERIFY_SUCCEEDED(session->Shutdown(30 * 1000));
            VERIFY_ARE_EQUAL(process.Get().Signal(9), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        {

            // Validate that new processes cannot be created after the VM is terminated.
            const char* executable = "dummy";
            WSLA_PROCESS_OPTIONS options{};
            options.CommandLine = &executable;
            options.Executable = executable;
            options.CommandLineCount = 1;

            wil::com_ptr<IWSLAProcess> process;
            int error{};
            VERIFY_ARE_EQUAL(session->CreateRootNamespaceProcess(&options, &process, &error), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
            VERIFY_ARE_EQUAL(error, -1);
        }
    }

    TEST_METHOD(CrashDumpCollection)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);
        int processId = 0;

        // Cache the existing crash dumps so we can check that a new one is created.
        auto crashDumpsDir = std::filesystem::temp_directory_path() / "wsla-crashes";
        std::set<std::filesystem::path> existingDumps;

        if (std::filesystem::exists(crashDumpsDir))
        {
            existingDumps = {std::filesystem::directory_iterator(crashDumpsDir), std::filesystem::directory_iterator{}};
        }

        // Create a stuck process and crash it.
        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            // Get the process id. This is need to identify the crash dump file.
            VERIFY_SUCCEEDED(process.Get().GetPid(&processId));

            // Send SIGSEV(11) to crash the process.
            VERIFY_SUCCEEDED(process.Get().Signal(11));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 11);
            VERIFY_ARE_EQUAL(result.Signalled, true);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            VERIFY_ARE_EQUAL(process.Get().Signal(9), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Dumps files are named with the format: wsl-crash-<sessionId>-<pid>-<processname>-<code>.dmp
        // Check if a new file was added in crashDumpsDir matching the pattern and not in existingDumps.
        std::string expectedPattern = std::format("wsl-crash-*-{}-_usr_bin_cat-11.dmp", processId);

        auto dumpFile = wsl::shared::retry::RetryWithTimeout<std::filesystem::path>(
            [crashDumpsDir, expectedPattern, existingDumps]() {
                for (const auto& entry : std::filesystem::directory_iterator(crashDumpsDir))
                {
                    const auto& filePath = entry.path();
                    if (existingDumps.find(filePath) == existingDumps.end() &&
                        PathMatchSpecA(filePath.filename().string().c_str(), expectedPattern.c_str()))
                    {
                        return filePath;
                    }
                }

                throw wil::ResultException(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            },
            std::chrono::milliseconds{100},
            std::chrono::seconds{10});

        // Ensure that the dump file is cleaned up after test completion.
        auto cleanup = wil::scope_exit([&] {
            if (std::filesystem::exists(dumpFile))
            {
                std::filesystem::remove(dumpFile);
            }
        });

        VERIFY_IS_TRUE(std::filesystem::exists(dumpFile));
        VERIFY_IS_TRUE(std::filesystem::file_size(dumpFile) > 0);
    }

    TEST_METHOD(VhdFormatting)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.RootVhd = testVhd.c_str();

        auto session = CreateSession(settings);

        constexpr auto formatedVhd = L"test-format-vhd.vhdx";

        // TODO: Replace this by a proper SDK method once it exists
        auto tokenInfo = wil::get_token_information<TOKEN_USER>();
        wsl::core::filesystem::CreateVhd(formatedVhd, 100 * 1024 * 1024, tokenInfo->User.Sid, false, false);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(session->Shutdown(30 * 1000));
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(formatedVhd));
        });

        // Format the disk.
        auto absoluteVhdPath = std::filesystem::absolute(formatedVhd).wstring();
        VERIFY_SUCCEEDED(session->FormatVirtualDisk(absoluteVhdPath.c_str()));

        // Validate error paths.
        VERIFY_ARE_EQUAL(session->FormatVirtualDisk(L"DoesNotExist.vhdx"), E_INVALIDARG);
        VERIFY_ARE_EQUAL(session->FormatVirtualDisk(L"C:\\DoesNotExist.vhdx"), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }
};