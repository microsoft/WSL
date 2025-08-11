/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWTests.cpp

Abstract:

    This file contains test cases for the LSW API.

--*/

#include "precomp.h"
#include "Common.h"
#include "LSWApi.h"

using namespace wsl::windows::common::registry;

using unique_vm = wil::unique_any<LSWVirtualMachineHandle, decltype(WslReleaseVirtualMachine), &WslReleaseVirtualMachine>;

class LSWTests
{
    WSL_TEST_CLASS(LSWTests)
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

    TEST_METHOD(GetVersion)
    {
        auto coinit = wil::CoInitializeEx();
        WSL_VERSION_INFORMATION version{};

        VERIFY_SUCCEEDED(WslGetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    std::tuple<int, wil::unique_handle, wil::unique_handle, wil::unique_handle> LaunchCommand(
        LSWVirtualMachineHandle vm, const std::vector<const char*>& command)
    {
        auto copiedCommand = command;
        if (copiedCommand.back() != nullptr)
        {
            copiedCommand.push_back(nullptr);
        }

        std::vector<ProcessFileDescriptorSettings> fds(3);
        fds[0].Number = 0;
        fds[1].Number = 1;
        fds[2].Number = 2;

        CreateProcessSettings createProcessSettings{};
        createProcessSettings.Executable = copiedCommand[0];
        createProcessSettings.Arguments = copiedCommand.data();
        createProcessSettings.FileDescriptors = fds.data();
        createProcessSettings.FdCount = 3;

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &createProcessSettings, &pid));

        return std::make_tuple(
            pid, wil::unique_handle{fds[0].Handle}, wil::unique_handle(fds[1].Handle), wil::unique_handle{fds[2].Handle});
    }

    int RunCommand(LSWVirtualMachineHandle vm, const std::vector<const char*>& command, int timeout = 600000)
    {
        auto [pid, _, __, ___] = LaunchCommand(vm, command);

        WaitResult result{};
        VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, timeout, &result));
        VERIFY_ARE_EQUAL(result.State, ProcessStateExited);
        return result.Code;
    }

    unique_vm CreateVm(const VirtualMachineSettings* settings)
    {
        unique_vm vm{};
        VERIFY_SUCCEEDED(WslCreateVirtualMachine(settings, &vm));

        DiskAttachSettings attachSettings{testVhd.c_str(), true};
        AttachedDiskInformation attachedDisk;

        VERIFY_SUCCEEDED(WslAttachDisk(vm.get(), &attachSettings, &attachedDisk));

        MountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", MountFlagsChroot | MountFlagsWriteableOverlayFs};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &mountSettings));

        MountSettings devmountSettings{nullptr, "/dev", "devtmpfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &devmountSettings));

        MountSettings sysmountSettings{nullptr, "/sys", "sysfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &sysmountSettings));

        MountSettings procmountSettings{nullptr, "/proc", "proc", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &procmountSettings));

        MountSettings ptsMountSettings{nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &ptsMountSettings));

        return vm;
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            VirtualMachineSettings settings{};
            settings.CPU.CpuCount = 4;
            settings.DisplayName = L"LSW";
            settings.Memory.MemoryMb = 1024;
            settings.Options.BootTimeoutMs = 30000;
            settings.Options.Dmesg = write.get();
            settings.Options.EnableEarlyBootDmesg = earlyBootLogging;

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

            std::thread thread(readDmesg);
            auto vm = CreateVm(&settings);
            auto detach = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                vm.reset();
                if (thread.joinable())
                {
                    thread.join();
                }
            });

            write.reset();

            std::vector<const char*> cmd = {"/bin/bash", "-c", "echo DmesgTest > /dev/kmsg"};
            VERIFY_ARE_EQUAL(RunCommand(vm.get(), cmd), 0);

            VERIFY_ARE_EQUAL(WslShutdownVirtualMachine(vm.get(), 30 * 1000), S_OK);
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

        std::promise<std::pair<VirtualMachineTerminationReason, std::wstring>> callbackInfo;

        auto callback = [](void* context, VirtualMachineTerminationReason reason, LPCWSTR details) -> HRESULT {
            auto* future = reinterpret_cast<std::promise<std::pair<VirtualMachineTerminationReason, std::wstring>>*>(context);

            future->set_value(std::make_pair(reason, details));

            return S_OK;
        };

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        settings.Options.TerminationCallback = callback;
        settings.Options.TerminationContext = &callbackInfo;

        auto vm = CreateVm(&settings);

        VERIFY_SUCCEEDED(WslShutdownVirtualMachine(vm.get(), 30 * 1000));

        auto future = callbackInfo.get_future();
        auto result = future.wait_for(std::chrono::seconds(10));
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, VirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }

    TEST_METHOD(CreateVmSmokeTest)
    {
        WSL2_TEST_ONLY();

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;

        auto vm = CreateVm(&settings);

        // Create a process and wait for it to exit
        {
            std::vector<const char*> commandLine{"/bin/sh", "-c", "echo $bar", nullptr};

            std::vector<ProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            std::vector<const char*> env{"bar=foo", nullptr};
            CreateProcessSettings createProcessSettings{};
            createProcessSettings.Executable = "/bin/sh";
            createProcessSettings.Arguments = commandLine.data();
            createProcessSettings.FileDescriptors = fds.data();
            createProcessSettings.Environment = env.data();
            createProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &createProcessSettings, &pid));

            LogInfo("pid: %lu", pid);

            std::vector<char> buffer(100);

            DWORD bytes{};
            if (!ReadFile(createProcessSettings.FileDescriptors[1].Handle, buffer.data(), (DWORD)buffer.size(), &bytes, nullptr))
            {
                LogError("ReadFile: %lu, handle: 0x%x", GetLastError(), createProcessSettings.FileDescriptors[1].Handle);
                VERIFY_FAIL();
            }

            VERIFY_ARE_EQUAL(buffer.data(), std::string("foo\n"));

            WaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, ProcessStateExited);
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Create a 'stuck' process and kill it
        {
            std::vector<const char*> commandLine{"/usr/bin/sleep", "100000", nullptr};

            std::vector<ProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            CreateProcessSettings createProcessSettings{};
            createProcessSettings.Executable = commandLine[0];
            createProcessSettings.Arguments = commandLine.data();
            createProcessSettings.FileDescriptors = fds.data();
            createProcessSettings.Environment = nullptr;
            createProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &createProcessSettings, &pid));

            // Verify that the process is in a running state
            WaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, ProcessStateRunning);

            // Verify that it can be killed.
            VERIFY_SUCCEEDED(WslSignalLinuxProcess(vm.get(), pid, 9));

            // Verify that the process is in a running state

            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, ProcessStateSignaled);
            VERIFY_ARE_EQUAL(result.Code, 9);
        }

        // Test various error paths
        {
            std::vector<const char*> commandLine{"dummy", "100000", nullptr};

            std::vector<ProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            CreateProcessSettings createProcessSettings{};
            createProcessSettings.Executable = commandLine[0];
            createProcessSettings.Arguments = commandLine.data();
            createProcessSettings.FileDescriptors = fds.data();
            createProcessSettings.Environment = nullptr;
            createProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_ARE_EQUAL(WslCreateLinuxProcess(vm.get(), &createProcessSettings, &pid), E_FAIL);

            WaitResult result{};
            VERIFY_ARE_EQUAL(WslWaitForLinuxProcess(vm.get(), 1234, 1000, &result), E_FAIL);
            VERIFY_ARE_EQUAL(result.State, ProcessStateUnknown);
        }
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Options.EnableDebugShell = true;
        settings.Networking.Mode = NetworkingModeNone;

        auto vm = CreateVm(&settings);

        std::vector<const char*> commandLine{"/bin/sh", nullptr};

        std::vector<ProcessFileDescriptorSettings> fds(2);
        fds[0].Number = 0;
        fds[0].Type = TerminalInput;
        fds[1].Number = 1;
        fds[1].Type = TerminalOutput;

        CreateProcessSettings createProcessSettings{};
        createProcessSettings.Executable = "/bin/sh";
        createProcessSettings.Arguments = commandLine.data();
        createProcessSettings.FileDescriptors = fds.data();
        createProcessSettings.FdCount = static_cast<ULONG>(fds.size());

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &createProcessSettings, &pid));

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(
                    createProcessSettings.FileDescriptors[1].Handle, buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(
                createProcessSettings.FileDescriptors[0].Handle, content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("#");
        writeTty("echo OK\n");
        validateTtyOutput(" echo OK\r\nOK");

        // Validate that the interactive process successfully starts
        wil::unique_handle process;
        VERIFY_SUCCEEDED(WslLaunchInteractiveTerminal(
            createProcessSettings.FileDescriptors[0].Handle, createProcessSettings.FileDescriptors[1].Handle, &process));

        // Exit the shell
        writeTty("exit\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(process.get(), 30 * 1000), WAIT_OBJECT_0);
    }

    TEST_METHOD(NATNetworking)
    {
        WSL2_TEST_ONLY();

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = NetworkingModeNAT;

        auto vm = CreateVm(&settings);

        // Validate that eth0 has an ip address
        VERIFY_ARE_EQUAL(
            RunCommand(
                vm.get(),
                {"/bin/bash",
                 "-c",
                 "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"}),
            0);

        // Verify that /etc/resolv.conf is configured
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}), 0);
    }

    TEST_METHOD(OpenFiles)
    {
        WSL2_TEST_ONLY();

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;

        auto vm = CreateVm(&settings);

        struct FileFd
        {
            int Fd;
            FileDescriptorType Flags;
            const char* Path;
        };

        auto createProcess = [&](std::vector<const char*> Args, const std::vector<FileFd>& Fds, std::optional<HRESULT> expectedError = {}) {
            Args.emplace_back(nullptr);

            std::vector<ProcessFileDescriptorSettings> fds;

            for (const auto& e : Fds)
            {
                fds.emplace_back(ProcessFileDescriptorSettings{e.Fd, e.Flags, e.Path, nullptr});
            }

            CreateProcessSettings createProcessSettings{};
            createProcessSettings.Executable = Args[0];
            createProcessSettings.Arguments = Args.data();
            createProcessSettings.FileDescriptors = fds.data();
            createProcessSettings.Environment = nullptr;
            createProcessSettings.FdCount = static_cast<uint32_t>(fds.size());

            int pid{};
            VERIFY_ARE_EQUAL(WslCreateLinuxProcess(vm.get(), &createProcessSettings, &pid), expectedError.value_or(S_OK));

            return fds;
        };

        {
            auto fds = createProcess({"/bin/cat"}, {{1, LinuxFileInput, "/proc/self/cmdline"}});

            VERIFY_ARE_EQUAL(ReadToString((SOCKET)fds[0].Handle), "/bin/cat");
        }
    }

    TEST_METHOD(NATPortMapping)
    {
        WSL2_TEST_ONLY();

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = NetworkingModeNAT;

        auto vm = CreateVm(&settings);

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
            auto [pid, in, out, err] = LaunchCommand(vm.get(), {"/bin/bash", "-c", cmd.c_str()});
            waitForOutput(err.get(), "listening on");

            return pid;
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
        PortMappingSettings port{1234, 80, AF_INET};
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
        PortMappingSettings portv6{1234, 80, AF_INET6};
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
        PortMappingSettings portv6Only{1235, 81, AF_INET6};
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &portv6Only));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &portv6Only));
        VERIFY_ARE_EQUAL(WslUnmapPort(vm.get(), &portv6Only), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(WslMapPort(vm.get(), &port));

        auto [pid, in, out, err] =
            LaunchCommand(vm.get(), {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"});
        waitForOutput(err.get(), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &port));
    }
};