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

using namespace wsl::windows::common::registry;

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
        WslVirtualMachineHandle vm, const std::vector<const char*>& command)
    {
        auto copiedCommand = command;
        if (copiedCommand.back() != nullptr)
        {
            copiedCommand.push_back(nullptr);
        }

        std::vector<WslProcessFileDescriptorSettings> fds(3);
        fds[0].Number = 0;
        fds[1].Number = 1;
        fds[2].Number = 2;

        WslCreateProcessSettings WslCreateProcessSettings{};
        WslCreateProcessSettings.Executable = copiedCommand[0];
        WslCreateProcessSettings.Arguments = copiedCommand.data();
        WslCreateProcessSettings.FileDescriptors = fds.data();
        WslCreateProcessSettings.FdCount = 3;

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &WslCreateProcessSettings, &pid));

        return std::make_tuple(
            pid, wil::unique_handle{fds[0].Handle}, wil::unique_handle(fds[1].Handle), wil::unique_handle{fds[2].Handle});
    }

    int RunCommand(WslVirtualMachineHandle vm, const std::vector<const char*>& command, int timeout = 600000)
    {
        auto [pid, _, __, ___] = LaunchCommand(vm, command);

        WslWaitResult result{};
        VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, timeout, &result));
        VERIFY_ARE_EQUAL(result.State, WslProcessStateExited);
        return result.Code;
    }

    unique_vm CreateVm(const WslVirtualMachineSettings* settings, const std::optional<LPCWSTR> rootfs = {})
    {
        unique_vm vm{};
        VERIFY_SUCCEEDED(WslCreateVirtualMachine(settings, &vm));

        WslDiskAttachSettings attachSettings{rootfs.value_or(testVhd.c_str()), true};
        WslAttachedDiskInformation attachedDisk;

        VERIFY_SUCCEEDED(WslAttachDisk(vm.get(), &attachSettings, &attachedDisk));

        WslMountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", WslMountFlagsChroot | WslMountFlagsWriteableOverlayFs};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &mountSettings));

        WslMountSettings devMountSettings{nullptr, "/dev", "devtmpfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &devMountSettings));

        WslMountSettings sysMountSettings{nullptr, "/sys", "sysfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &sysMountSettings));

        WslMountSettings procMountSettings{nullptr, "/proc", "proc", "", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &procMountSettings));

        WslMountSettings ptsMountSettings{nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", false};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &ptsMountSettings));

        return vm;
    }

    TEST_METHOD(AttachDetach)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        auto vm = CreateVm(&settings);

#ifdef WSL_DEV_INSTALL_PATH

        auto vhdPath = std::filesystem::path(WSL_DEV_INSTALL_PATH) / "system.vhd";
#else

        auto msiPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(msiPath.has_value());

        auto vhdPath = std::filesystem::path(msiPath.value()) / "system.vhd";

#endif

        auto blockDeviceExists = [&](ULONG Lun) {
            std::string device = std::format("/sys/bus/scsi/devices/0:0:0:{}", Lun);
            std::vector<const char*> cmd{"/usr/bin/test", "-d", device.c_str()};
            return RunCommand(vm.get(), cmd) == 0;
        };

        // Attach the disk.
        WslDiskAttachSettings attachSettings{vhdPath.c_str(), true};
        WslAttachedDiskInformation attachedDisk{};
        VERIFY_SUCCEEDED(WslAttachDisk(vm.get(), &attachSettings, &attachedDisk));
        VERIFY_IS_TRUE(blockDeviceExists(attachedDisk.ScsiLun));

        // Mount it to /mnt.
        WslMountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro"};
        VERIFY_SUCCEEDED(WslMount(vm.get(), &mountSettings));

        // Validate that the mountpoint is present.
        std::vector<const char*> cmd{"/usr/bin/mountpoint", "/mnt"};
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), cmd), 0L);

        // Unmount /mnt.
        VERIFY_SUCCEEDED(WslUnmount(vm.get(), "/mnt"));
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), cmd), 32L);

        // Verify that unmount fails now.
        VERIFY_ARE_EQUAL(WslUnmount(vm.get(), "/mnt"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        // Detach the disk.
        VERIFY_SUCCEEDED(WslDetachDisk(vm.get(), attachedDisk.ScsiLun));
        VERIFY_IS_FALSE(blockDeviceExists(attachedDisk.ScsiLun));

        // Verify that disk can't be detached twice.
        VERIFY_ARE_EQUAL(WslDetachDisk(vm.get(), attachedDisk.ScsiLun), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        // Validate that invalid flags return E_INVALIDARG.
        WslMountSettings invalidFlagSettings{"/dev/sda", "/mnt", "ext4", "ro", 0x4};
        VERIFY_ARE_EQUAL(WslMount(vm.get(), &invalidFlagSettings), E_INVALIDARG);

        invalidFlagSettings.Flags = 0xff;
        VERIFY_ARE_EQUAL(WslMount(vm.get(), &invalidFlagSettings), E_INVALIDARG);
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            WslVirtualMachineSettings settings{};
            settings.CPU.CpuCount = 4;
            settings.DisplayName = L"WSLA";
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
    }

    TEST_METHOD(CreateVmSmokeTest)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;

        auto vm = CreateVm(&settings);

        // Create a process and wait for it to exit
        {
            std::vector<const char*> commandLine{"/bin/sh", "-c", "echo $bar", nullptr};

            std::vector<WslProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            std::vector<const char*> env{"bar=foo", nullptr};
            WslCreateProcessSettings WslCreateProcessSettings{};
            WslCreateProcessSettings.Executable = "/bin/sh";
            WslCreateProcessSettings.Arguments = commandLine.data();
            WslCreateProcessSettings.FileDescriptors = fds.data();
            WslCreateProcessSettings.Environment = env.data();
            WslCreateProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid));

            LogInfo("pid: %lu", pid);

            std::vector<char> buffer(100);

            DWORD bytes{};
            if (!ReadFile(WslCreateProcessSettings.FileDescriptors[1].Handle, buffer.data(), (DWORD)buffer.size(), &bytes, nullptr))
            {
                LogError("ReadFile: %lu, handle: 0x%x", GetLastError(), WslCreateProcessSettings.FileDescriptors[1].Handle);
                VERIFY_FAIL();
            }

            VERIFY_ARE_EQUAL(buffer.data(), std::string("foo\n"));

            WslWaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateExited);
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Create a 'stuck' process and kill it
        {
            std::vector<const char*> commandLine{"/usr/bin/sleep", "100000", nullptr};

            std::vector<WslProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            WslCreateProcessSettings WslCreateProcessSettings{};
            WslCreateProcessSettings.Executable = commandLine[0];
            WslCreateProcessSettings.Arguments = commandLine.data();
            WslCreateProcessSettings.FileDescriptors = fds.data();
            WslCreateProcessSettings.Environment = nullptr;
            WslCreateProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid));

            // Verify that the process is in a running state
            WslWaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateRunning);

            // Verify that the process can still be waited for
            result = {};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateRunning);

            result = {};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 0, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateRunning);

            // Verify that it can be killed.
            VERIFY_SUCCEEDED(WslSignalLinuxProcess(vm.get(), pid, 9));

            // Verify that the process is in a running state

            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateSignaled);
            VERIFY_ARE_EQUAL(result.Code, 9);
        }

        // Test various error paths
        {
            std::vector<const char*> commandLine{"dummy", "100000", nullptr};

            std::vector<WslProcessFileDescriptorSettings> fds(3);
            fds[0].Number = 0;
            fds[1].Number = 1;
            fds[2].Number = 2;

            WslCreateProcessSettings WslCreateProcessSettings{};
            WslCreateProcessSettings.Executable = commandLine[0];
            WslCreateProcessSettings.Arguments = commandLine.data();
            WslCreateProcessSettings.FileDescriptors = fds.data();
            WslCreateProcessSettings.Environment = nullptr;
            WslCreateProcessSettings.FdCount = 3;

            int pid = -1;
            VERIFY_ARE_EQUAL(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid), E_FAIL);

            WslWaitResult result{};
            VERIFY_ARE_EQUAL(WslWaitForLinuxProcess(vm.get(), 1234, 1000, &result), E_FAIL);
            VERIFY_ARE_EQUAL(result.State, WslProcessStateUnknown);
        }
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Options.EnableDebugShell = true;
        settings.Networking.Mode = WslNetworkingModeNone;

        auto vm = CreateVm(&settings);

        std::vector<const char*> commandLine{"/bin/sh", nullptr};
        std::vector<WslProcessFileDescriptorSettings> fds(3);
        fds[0].Number = 0;
        fds[0].Type = WslFdTypeTerminalInput;
        fds[1].Number = 1;
        fds[1].Type = WslFdTypeTerminalOutput;
        fds[2].Number = 2;
        fds[2].Type = WslFdTypeTerminalControl;

        const char* env[] = {"TERM=xterm-256color", nullptr};
        WslCreateProcessSettings WslCreateProcessSettings{};
        WslCreateProcessSettings.Executable = "/bin/sh";
        WslCreateProcessSettings.Arguments = commandLine.data();
        WslCreateProcessSettings.FileDescriptors = fds.data();
        WslCreateProcessSettings.FdCount = static_cast<ULONG>(fds.size());
        WslCreateProcessSettings.Environment = env;

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid));

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(
                    WslCreateProcessSettings.FileDescriptors[1].Handle, buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(
                WslCreateProcessSettings.FileDescriptors[0].Handle, content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("#");
        writeTty("echo OK\n");
        validateTtyOutput(" echo OK\r\nOK");

        // Validate that the interactive process successfully starts
        wil::unique_handle process;
        VERIFY_SUCCEEDED(WslLaunchInteractiveTerminal(
            WslCreateProcessSettings.FileDescriptors[0].Handle,
            WslCreateProcessSettings.FileDescriptors[1].Handle,
            WslCreateProcessSettings.FileDescriptors[2].Handle,
            &process));

        // Exit the shell
        writeTty("exit\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(process.get(), 30000 * 1000), WAIT_OBJECT_0);
    }

    TEST_METHOD(NATNetworking)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNAT;

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

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;

        auto vm = CreateVm(&settings);

        struct FileFd
        {
            int Fd;
            WslFdType Flags;
            const char* Path;
        };

        auto createProcess = [&](std::vector<const char*> Args, const std::vector<FileFd>& Fds, HRESULT expectedError = S_OK) {
            Args.emplace_back(nullptr);

            std::vector<WslProcessFileDescriptorSettings> fds;

            for (const auto& e : Fds)
            {
                fds.emplace_back(WslProcessFileDescriptorSettings{e.Fd, e.Flags, e.Path, nullptr});
            }

            WslCreateProcessSettings WslCreateProcessSettings{};
            WslCreateProcessSettings.Executable = Args[0];
            WslCreateProcessSettings.Arguments = Args.data();
            WslCreateProcessSettings.FileDescriptors = fds.data();
            WslCreateProcessSettings.FdCount = static_cast<DWORD>(fds.size());

            int pid{};
            VERIFY_ARE_EQUAL(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid), expectedError);

            std::vector<wil::unique_handle> handles;

            for (const auto& e : fds)
            {
                handles.emplace_back(e.Handle);
            }

            return std::make_pair(std::move(handles), pid);
        };

        auto wait = [&](int pid) {
            WslWaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm.get(), pid, INFINITE, &result));
            VERIFY_ARE_EQUAL(result.State, WslProcessStateExited);

            return result.Code;
        };

        {
            auto [fds, pid] =
                createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WslFdTypeDefault, nullptr}});
            VERIFY_ARE_EQUAL(ReadToString((SOCKET)fds[1].get()), "cat\n");
            VERIFY_ARE_EQUAL(wait(pid), 0);
        }

        {
            auto read = [&]() {
                auto [readFds, readPid] =
                    createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileInput, "/tmp/output"}, {1, WslFdTypeDefault, nullptr}});
                VERIFY_ARE_EQUAL(wait(readPid), 0);

                auto content = ReadToString((SOCKET)readFds[1].get());

                return content;
            };

            // Write to a new file.
            auto [fds, pid] = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr},
                 {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput | WslFdTypeLinuxFileCreate), "/tmp/output"}});

            constexpr auto content = "TestOutput";
            VERIFY_IS_TRUE(WriteFile(fds[0].get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            fds.clear();
            VERIFY_ARE_EQUAL(wait(pid), 0);

            VERIFY_ARE_EQUAL(read(), content);

            // Append content to the same file
            auto [appendFds, appendPid] = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr},
                 {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput | WslFdTypeLinuxFileAppend), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(appendFds[0].get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            appendFds.clear();
            VERIFY_ARE_EQUAL(wait(appendPid), 0);

            VERIFY_ARE_EQUAL(read(), std::format("{}{}", content, content));

            // Truncate the file
            auto [truncFds, truncPid] = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeDefault, nullptr}, {1, static_cast<WslFdType>(WslFdTypeLinuxFileOutput), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(truncFds[0].get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            truncFds.clear();
            VERIFY_ARE_EQUAL(wait(truncPid), 0);

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
            auto [fds, pid] = createProcess(
                {"/bin/cat"},
                {{0, WslFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WslFdTypeLinuxFileInput, "/tmp/output"}, {2, WslFdTypeDefault, nullptr}});

            VERIFY_ARE_EQUAL(ReadToString((SOCKET)fds[2].get()), "/bin/cat: write error: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(wait(pid), 1);
        }

        {
            auto [fds, pid] = createProcess({"/bin/cat"}, {{0, WslFdTypeLinuxFileOutput, "/tmp/output"}, {2, WslFdTypeDefault, nullptr}});

            VERIFY_ARE_EQUAL(ReadToString((SOCKET)fds[1].get()), "/bin/cat: standard output: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(wait(pid), 1);
        }
    }

    TEST_METHOD(NATPortMapping)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNAT;

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

        auto [pid, in, out, err] =
            LaunchCommand(vm.get(), {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"});
        waitForOutput(err.get(), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(WslUnmapPort(vm.get(), &port));
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNone;

        auto vm = CreateVm(&settings);

        auto [pid, stdinFd, _, __] = LaunchCommand(vm.get(), {"/bin/cat"});

        // Create a 'stuck' thread, waiting for cat to exit

        std::thread stuckThread([&]() {
            WslWaitResult result{};
            WslWaitForLinuxProcess(vm.get(), pid, INFINITE, &result);
        });

        // Stop the service
        StopWslaService();

        // Verify that the thread is unstuck
        stuckThread.join();
    }

    TEST_METHOD(WindowsMounts)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNone;

        auto vm = CreateVm(&settings);

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
            VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt && sync"}), 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(WslUnmountWindowsFolder(vm.get(), "/win-path"));
            expectMount("/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(WslMountWindowsFolder(vm.get(), testFolder.c_str(), "/win-path", true));
            expectMount("/win-path", "/win-path*9p*rw,relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            // Validate that folder is not writeable from linux
            VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/bash", "-c", "echo -n content > /win-path/file.txt"}), 1);

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

            VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/bin/umount", "/win-path"}), 0);
            VERIFY_SUCCEEDED(WslUnmountWindowsFolder(vm.get(), "/win-path"));
        }
    }

    // This test case validates that no file descriptors are leaked to user processes.
    TEST_METHOD(Fd)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNone;

        auto vm = CreateVm(&settings);

        std::vector<WslProcessFileDescriptorSettings> fds(1);
        fds[0].Number = 1;
        fds[0].Type = WslFdTypeDefault;

        const char* args[] = {"/bin/bash", "-c", "echo /proc/self/fd/* && readlink /proc/self/fd/*", nullptr};
        WslCreateProcessSettings WslCreateProcessSettings{};
        WslCreateProcessSettings.Executable = "/bin/bash";
        WslCreateProcessSettings.Arguments = args;
        WslCreateProcessSettings.FileDescriptors = fds.data();
        WslCreateProcessSettings.FdCount = 1;

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm.get(), &WslCreateProcessSettings, &pid));

        wil::unique_socket output{(SOCKET)fds[0].Handle};
        auto result = ReadToString(output.get());

        // Note: fd/0 is opened readlink to read the actual content of /proc/fd.
        if (!PathMatchSpecA(result.c_str(), "/proc/self/fd/0 /proc/self/fd/1\nsocket:[*]\n"))
        {
            LogInfo("Found additional fds: %hs", result.c_str());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(GPU)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNAT;
        settings.GPU.Enable = true;

        auto vm = CreateVm(&settings);

        // Validate that the GPU device is available.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/bash", "-c", "test -c /dev/dxg"}), 0);

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

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        WslVirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"WSLA";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Networking.Mode = WslNetworkingModeNone;

        // Use the system distro vhd for modprobe & lsmod.

#ifdef WSL_SYSTEM_DISTRO_PATH

        auto rootfs = std::filesystem::path(TEXT(WSL_SYSTEM_DISTRO_PATH));

#else
        auto rootfs = std::filesystem::path(wsl::windows::common::wslutil::GetMsiPackagePath().value()) / L"system.vhd";

#endif

        auto vm = CreateVm(&settings, rootfs.c_str());

        // Sanity check.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}), 1);

        // Validate that modules can be loaded.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/usr/sbin/modprobe", "xsk_diag"}), 0);

        // Validate that xsk_diag is now loaded.
        VERIFY_ARE_EQUAL(RunCommand(vm.get(), {"/bin/bash", "-c", "lsmod | grep ^xsk_diag"}), 0);
    }
};