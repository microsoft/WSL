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

class LSWTests
{
    WSL_TEST_CLASS(LSWTests)
    wil::unique_couninitialize_call coinit = wil::CoInitializeEx();
    WSADATA Data;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

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

    int RunCommand(LSWVirtualMachineHandle vm, const std::vector<const char*>& command)
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

        WaitResult result{};
        VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, 1000, &result));
        VERIFY_ARE_EQUAL(result.State, ProcessStateExited);
        return result.Code;
    }

    LSWVirtualMachineHandle CreateVm(const VirtualMachineSettings* settings)
    {
        LSWVirtualMachineHandle vm{};
        VERIFY_SUCCEEDED(WslCreateVirualMachine(settings, (LSWVirtualMachineHandle*)&vm));

#ifdef WSL_SYSTEM_DISTRO_PATH

        std::wstring systemdDistroDiskPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else

        auto systemdDistroDiskPath = std::format(L"{}/system.vhd", wsl::windows::common::wslutil::GetMsiPackagePath().value());
#endif

        DiskAttachSettings attachSettings{systemdDistroDiskPath.c_str(), true};
        AttachedDiskInformation attachedDisk;

        VERIFY_SUCCEEDED(WslAttachDisk(vm, &attachSettings, &attachedDisk));

        MountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", MountFlagsChroot | MountFlagsWriteableOverlayFs};
        VERIFY_SUCCEEDED(WslMount(vm, &mountSettings));

        MountSettings devmountSettings{nullptr, "/dev", "devtmpfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm, &devmountSettings));

        MountSettings sysmountSettings{nullptr, "/sys", "sysfs", "", false};
        VERIFY_SUCCEEDED(WslMount(vm, &sysmountSettings));

        MountSettings procmountSettings{nullptr, "/proc", "proc", "", false};
        VERIFY_SUCCEEDED(WslMount(vm, &procmountSettings));

        MountSettings ptsMountSettings{nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", false};
        VERIFY_SUCCEEDED(WslMount(vm, &ptsMountSettings));

        return vm;
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        auto [read, write] = CreateSubprocessPipe(false, false);

        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        settings.Options.Dmesg = write.get();

        std::vector<char> dmesgContent;

        auto readDmesg = [&]() {
            DWORD Offset = 0;

            constexpr auto bufferSize = 1024;
            while (true)
            {
                dmesgContent.resize(Offset + bufferSize);

                DWORD Read{};
                if (!ReadFile(read.get(), &dmesgContent[Offset], bufferSize, &Read, nullptr))
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
        write.reset();

        auto detach = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            WslReleaseVirtualMachine(vm);
            if (thread.joinable())
            {
                thread.join();
            }
        });

        std::vector<const char*> cmd = {"/bin/bash", "-c", "echo DmesgTest > /dev/kmsg"};
        VERIFY_ARE_EQUAL(RunCommand(vm, cmd), 0);

        VERIFY_ARE_EQUAL(WslShutdownVirtualMachine(vm, 30 * 1000), S_OK);
        detach.reset();

        auto contentString = std::string(dmesgContent.begin(), dmesgContent.end());

        VERIFY_ARE_NOT_EQUAL(contentString.find("Run /init as init process"), std::string::npos);
        VERIFY_ARE_NOT_EQUAL(contentString.find("DmesgTest"), std::string::npos);
    }

    TEST_METHOD(TerminationCallback)
    {
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

        VERIFY_SUCCEEDED(WslShutdownVirtualMachine(vm, 30 * 1000));

        auto future = callbackInfo.get_future();
        auto result = future.wait_for(std::chrono::seconds(10));
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, VirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");

        WslReleaseVirtualMachine(vm);
    }

    TEST_METHOD(CreateVmSmokeTest)
    {
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
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &createProcessSettings, &pid));

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
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, 1000, &result));
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
            VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &createProcessSettings, &pid));

            // Verify that the process is in a running state
            WaitResult result{};
            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, ProcessStateRunning);

            // Verify that it can be killed.
            VERIFY_SUCCEEDED(WslSignalLinuxProcess(vm, pid, 9));

            // Verify that the process is in a running state

            VERIFY_SUCCEEDED(WslWaitForLinuxProcess(vm, pid, 1000, &result));
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
            VERIFY_ARE_EQUAL(WslCreateLinuxProcess(vm, &createProcessSettings, &pid), E_FAIL);

            WaitResult result{};
            VERIFY_ARE_EQUAL(WslWaitForLinuxProcess(vm, 1234, 1000, &result), E_FAIL);
            VERIFY_ARE_EQUAL(result.State, ProcessStateUnknown);
        }

        WslReleaseVirtualMachine(vm);
    }

    TEST_METHOD(InteractiveShell)
    {
        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 2048;
        settings.Options.BootTimeoutMs = 30 * 1000;
        settings.Options.EnableDebugShell = true;
        settings.Networking.Mode = NetworkingModeNone;

        auto vm = CreateVm(&settings);

        /*std::vector<const char*> cmd = {"/usr/bin/setsid", "/sbin/agetty", "-w", "-L", "hvc1", "-a", "root", nullptr};

        CreateProcessSettings options{};
        options.Executable = "/usr/bin/setsid";
        options.Arguments = cmd.data();
        options.FdCount = 0;

        int pid = -1;
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &options, &pid));

        wil::unique_handle processs;
        VERIFY_SUCCEEDED(WslLaunchDebugShell(vm, &processs));*/

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
        VERIFY_SUCCEEDED(WslCreateLinuxProcess(vm, &createProcessSettings, &pid));

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
        validateTtyOutput("sh-5.1#");
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
                vm,
                {"/bin/bash",
                 "-c",
                 "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"}),
            0);

        // Verify that /etc/resolv.conf is configured
        VERIFY_ARE_EQUAL(RunCommand(vm, {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}), 0);
    }
};