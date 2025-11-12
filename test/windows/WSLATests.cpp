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

using namespace wsl::windows::common::registry;
using wsl::windows::common::ProcessFlags;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;

using unique_vm = wil::unique_any<WslVirtualMachineHandle, decltype(WslReleaseVirtualMachine), &WslReleaseVirtualMachine>;

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

    wil::com_ptr<IWSLASession> CreateSession(const VIRTUAL_MACHINE_SETTINGS& vmSettings, const std::optional<std::wstring>& vhd = {})
    {
        wil::com_ptr<IWSLAUserSession> userSession;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        WSLA_SESSION_SETTINGS settings{L"wsla-test"};
        wil::com_ptr<IWSLASession> session;

        VERIFY_SUCCEEDED(userSession->CreateSession(&settings, &vmSettings, &session));

        // TODO: remove once the VM is wired to mount its rootfs inside WSLASession
        wil::com_ptr<IWSLAVirtualMachine> virtualMachine;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&virtualMachine));

        wsl::windows::common::security::ConfigureForCOMImpersonation(virtualMachine.get());

        wil::unique_cotaskmem_ansistring diskDevice;
        ULONG Lun{};
        THROW_IF_FAILED(virtualMachine->AttachDisk(vhd.value_or(testVhd).c_str(), true, &diskDevice, &Lun));

        THROW_IF_FAILED(virtualMachine->Mount(diskDevice.get(), "/mnt", "ext4", "ro", WslMountFlagsChroot | WslMountFlagsWriteableOverlayFs));
        THROW_IF_FAILED(virtualMachine->Mount(nullptr, "/dev", "devtmpfs", "", 0));
        THROW_IF_FAILED(virtualMachine->Mount(nullptr, "/sys", "sysfs", "", 0));
        THROW_IF_FAILED(virtualMachine->Mount(nullptr, "/proc", "proc", "", 0));
        THROW_IF_FAILED(virtualMachine->Mount(nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", 0));

        return session;
    }

    TEST_METHOD(GetVersion)
    {
        auto coinit = wil::CoInitializeEx();
        WSL_VERSION_INFORMATION version{};

        VERIFY_SUCCEEDED(WslGetVersion(&version));

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
                    "Command: %hs didn't received an unexpected signal: %i. Stdout: '%hs', Stderr: '%hs'",
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

    /*
    TODO: Implement once available.
    TEST_METHOD(TerminationCallback)
    {
        WSL2_TEST_ONLY();

        std::promise<std::pair<WslVirtualMachineTerminationReason, std::wstring>> callbackInfo;

        auto callback = [](void* context, WslVirtualMachineTerminationReason reason, LPCWSTR details) -> HRESULT {
            auto* future = reinterpret_cast<std::promise<std::pair<WslVirtualMachineTerminationReason, std::wstring>>*>(context);

            future->set_value(std::make_pair(reason, details));

            return S_OK;
        };

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        settings.Options.TerminationCallback = callback;
        settings.Options.TerminationContext = &callbackInfo;

        auto vm = CreateVm(&settings);

        VERIFY_SUCCEEDED(WslShutdownVirtualMachine(vm.get(), 30 * 1000));

        auto future = callbackInfo.get_future();
        auto result = future.wait_for(std::chrono::seconds(10));
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, WslVirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }*/

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

        auto session = CreateSession(settings);

        WSLAProcessLauncher launcher("/bin/sh", {"/bin/sh"}, {"TERM=xterm-256color"}, ProcessFlags::None);
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WslFdTypeTerminalInput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WslFdTypeTerminalOutput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WslFdTypeTerminalControl});

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

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 100));
    }

    TEST_METHOD(NATNetworking)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = WslNetworkingModeNAT;

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
        settings.NetworkingMode = WslNetworkingModeNAT;
        settings.EnableDnsTunneling = true;

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

    TEST_METHOD(OpenFiles)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

        auto session = CreateSession(settings);

        struct FileFd
        {
            int Fd;
            WslFdType Flags;
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
            auto process = createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WslFdTypeDefault, nullptr}});

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Output[1], "cat\n");
        }

        {

            auto read = [&]() {
                auto process = createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileInput, "/tmp/output"}, {1, WslFdTypeDefault, nullptr}});
                return process->WaitAndCaptureOutput().Output[1];
            };

            // Write to a new file.
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr},
                 {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput | WslFdTypeLinuxFileCreate), "/tmp/output"}});

            constexpr auto content = "TestOutput";
            VERIFY_IS_TRUE(WriteFile(process->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);

            // Append content to the same file
            auto appendProcess = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr},
                 {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput | WslFdTypeLinuxFileAppend), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(appendProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(appendProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), std::format("{}{}", content, content));

            // Truncate the file
            auto trunProcess = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr}, {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(trunProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(trunProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);
        }

        // Test various error paths
        {
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeLinuxFileOutput), "/tmp/DoesNotExist"}}, E_FAIL);
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeDefault), "should-be-null"}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeDefault | WslFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WslFdType>(WslFdTypeLinuxFileInput | WslFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
        }

        // Validate that read & write modes are respected
        {
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WslFdTypeLinuxFileInput, "/tmp/output"}, {2, WslFdTypeDefault, nullptr}});

            auto result = process->WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[2], "/bin/cat: write error: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(result.Code, 1);
        }

        {
            auto process = createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileOutput, "/tmp/output"}, {2, WslFdTypeDefault, nullptr}});
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
        settings.NetworkingMode = WslNetworkingModeNAT;

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
        WslPortMappingSettings port{1234, 80, AF_INET};
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &port));

        // Validate that the same port can't be bound twice
        VERIFY_ARE_EQUAL(WslMapPort(vm.get(), &port), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Check simple case
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that same port mapping can be reused
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that the connection is immediately reset if the port is not bound on the linux side
        expectContent(1234, AF_INET, "");

        // Add a ipv6 binding
        WslPortMappingSettings portv6{1234, 80, AF_INET6};
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &portv6));

        // Validate that ipv6 bindings work as well.
        listen(80, "port80ipv6", true);
        expectContent(1234, AF_INET6, "port80ipv6");

        // Unmap the ipv4 port
        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &port));
        expectNotBound(1234, AF_INET);

        // Verify that a proper error is returned if the mapping doesn't exist
        VERIFY_ARE_EQUAL(WslUnmapPort(vm.get(), &port), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        // Unmap the v6 port
        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &portv6));
        expectNotBound(1234, AF_INET6);

        // Map another port as v6 only
        WslPortMappingSettings portv6Only{1235, 81, AF_INET6};
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &portv6Only));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &portv6Only));
        VERIFY_ARE_EQUAL(WslUnmapPort(vm.get(), &portv6Only), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &port));

        auto process =
            WSLAProcessLauncher{"/usr/bin/socat", {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"}}
                .Launch(*session);

        waitForOutput(process.GetStdHandle(2).get(), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &port));
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

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
            VERIFY_SUCCEEDED(WslMountWindowsFolder(vm.get(), testFolder.c_str(), "/win-path", false));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            // Validate that mount can't be stacked on each other
            VERIFY_ARE_EQUAL(WslMountWindowsFolder(vm.get(), testFolder.c_str(), "/win-path", false), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that folder is writeable from linux
            ExpectCommandResult(session.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt && sync"}, 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(WslUnmountWindowsFolder(vm.get(), "/win-path"));
            expectMount("/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(WslMountWindowsFolder(vm.get(), testFolder.c_str(), "/win-path", true));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            // Validate that folder is not writeable from linux
            ExpectCommandResult(session.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(WslUnmountWindowsFolder(vm.get(), "/win-path"));
            expectMount("/win-path", {});
        }

        // Validate various error paths
        {
            VERIFY_ARE_EQUAL(WslMountWindowsFolder(vm.get(), L"relative-path", "/win-path", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(WslMountWindowsFolder(vm.get(), L"C:\\does-not-exist", "/win-path", true), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            VERIFY_ARE_EQUAL(WslUnmountWindowsFolder(vm.get(), "/not-mounted"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(WslUnmountWindowsFolder(vm.get(), "/proc"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(WslMountWindowsFolder(vm.get(), testFolder.c_str(), "/win-path", true));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(WslUnmountWindowsFolder(vm.get(), "/win-path"));
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

    /*
    TODO: Enable once GPU is available in new api
    TEST_METHOD(GPU)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.EnableGPU = true;

        auto session = CreateSession(settings);

        wil::com_ptr<IWSLAVirtualMachine> vm;
        VERIFY_SUCCEEDED(session->GetVirtualMachine(&vm));

        // Validate that the GPU device is available.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "test -c /dev/dxg"}, 0);


        // Validate that invalid flags return E_INVALIDARG
        {
            VERIFY_ARE_EQUAL(WslMountGpuLibraries(vm.get(), "/usr/lib/wsl/lib", "/usr/lib/wsl/drivers", WslMountFlagsChroot), E_INVALIDARG);
            VERIFY_ARE_EQUAL(WslMountGpuLibraries(vm.get(), "/usr/lib/wsl/lib", "/usr/lib/wsl/drivers", static_cast<WslMountFlags>(1024)), E_INVALIDARG);
        }

        // Validate GPU mounts
        VERIFY_SUCCEEDED(WslMountGpuLibraries(vm.get(), "/usr/lib/wsl/lib", "/usr/lib/wsl/drivers", WslMountFlagsNone));

        auto expectMount = [&](const std::string& target, const std::optional<std::string>& options) {
            auto cmd = std::format("set -o pipefail ; findmnt '{}' | tail  -n 1", target);
            auto [pid, in, out, err] = LaunchCommand(vm.get(), {"/bin/bash", "-c", cmd.c_str()});

            auto output = ReadToString((SOCKET)out.get());
            auto error = ReadToString((SOCKET)err.get());

            WslWaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, INFINITE, &result));
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

        // Validate that the mount points arenot writeable.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}), 1L);
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}), 1L);

        // Create a writeable mount point.
        VERIFY_SUCCEEDED(WslMountGpuLibraries(vm.get(), "/usr/lib/wsl/lib-rw", "/usr/lib/wsl/drivers-rw", WslMountFlagsWriteableOverlayFs));
        expectMount(
            "/usr/lib/wsl/drivers-rw",
            "/usr/lib/wsl/drivers-rw "
            "none*overlay*rw,relatime,lowerdir=/usr/lib/wsl/drivers-rw,upperdir=/usr/lib/wsl/drivers-rw-rw/rw/upper,workdir=/usr/"
            "lib/wsl/drivers-rw-rw/rw/work*");
        expectMount(
            "/usr/lib/wsl/lib-rw",
            "/usr/lib/wsl/lib-rw none*overlay "
            "rw,relatime,lowerdir=/usr/lib/wsl/lib-rw,upperdir=/usr/lib/wsl/lib-rw-rw/rw/upper,workdir=/usr/lib/wsl/lib-rw-rw/rw/"
            "work*");

        // Verify that the mountpoints are actually writeable.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib-rw/test"}), 0L);
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers-rw/test"}), 0L);

        // Validate that trying to mount the shares without GPU support disabled fails.
        {
            settings.GPU.Enable = false;
            auto vm = CreateVm(&settings);

            VERIFY_ARE_EQUAL(
                WslMountGpuLibraries(vm.get(), "/usr/lib/wsl/lib", "/usr/lib/wsl/drivers", WslMountFlagsNone),
                HRESULT_FROM_WIN32(ERROR_INVALID_CONFIG_VALUE));
        }
    }

    */

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

        // Use the system distro vhd for modprobe & lsmod.

#ifdef WSL_SYSTEM_DISTRO_PATH

        auto rootfs = std::filesystem::path(TEXT(WSL_SYSTEM_DISTRO_PATH));

#else
        auto rootfs = std::filesystem::path(wsl::windows::common::wslutil::GetMsiPackagePath().value()) / L"system.vhd";

#endif
        auto session = CreateSession(settings, rootfs);

        // Sanity check.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(session.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(session.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    TEST_METHOD(CreateSessionSmokeTest)
    {
        WSL2_TEST_ONLY();

        wil::com_ptr<IWSLAUserSession> userSession;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        WSLA_SESSION_SETTINGS settings{L"my-display-name"};
        wil::com_ptr<IWSLASession> session;

        VIRTUAL_MACHINE_SETTINGS vmSettings{};
        vmSettings.BootTimeoutMs = 30 * 1000;
        vmSettings.DisplayName = L"WSLA";
        vmSettings.MemoryMb = 2048;
        vmSettings.CpuCount = 4;
        vmSettings.NetworkingMode = WslNetworkingModeNone;
        vmSettings.EnableDebugShell = true;

        VERIFY_SUCCEEDED(userSession->CreateSession(&settings, &vmSettings, &session));

        wil::unique_cotaskmem_string returnedDisplayName;
        VERIFY_SUCCEEDED(session->GetDisplayName(&returnedDisplayName));

        VERIFY_ARE_EQUAL(returnedDisplayName.get(), std::wstring(L"my-display-name"));
    }

    TEST_METHOD(CreateRootNamespaceProcess)
    {
        WSL2_TEST_ONLY();

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

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

            // Send SIGKILL(9) to the process.
            VERIFY_SUCCEEDED(process.Get().Signal(9));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 9);
            VERIFY_ARE_EQUAL(result.Signalled, true);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");
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
    }
};