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
        WSL_VERSION version{};

        VERIFY_SUCCEEDED(GetWslVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    int RunCommand(LSWVirtualMachineHandle vm, std::vector<const char*>& command)
    {

        if (command.back() != nullptr)
        {
            command.push_back(nullptr);
        }

        std::vector<ProcessFileDescriptorSettings> fds(3);
        fds[0].Number = 0;
        fds[1].Number = 1;
        fds[2].Number = 2;

        CreateProcessSettings createProcessSettings{};
        createProcessSettings.Executable = command[0];
        createProcessSettings.Arguments = command.data();
        createProcessSettings.FileDescriptors = fds.data();
        createProcessSettings.FdCount = 3;

        int pid = -1;
        VERIFY_SUCCEEDED(CreateLinuxProcess((LSWVirtualMachineHandle*)vm, &createProcessSettings, &pid));

        WaitResult result{};
        VERIFY_SUCCEEDED(WaitForLinuxProcess((LSWVirtualMachineHandle*)vm, pid, 1000, &result));
        VERIFY_ARE_EQUAL(result.State, ProcessStateExited);
        return result.Code;
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        auto [read, write] = CreateSubprocessPipe(false, false);

        LSWVirtualMachineHandle vm{};
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
        thread.detach();

        VERIFY_SUCCEEDED(CreateVirualMachine(&settings, (LSWVirtualMachineHandle*)&vm));
        write.reset();

#ifdef WSL_SYSTEM_DISTRO_PATH

        std::wstring systemdDistroDiskPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else

        auto systemdDistroDiskPath = std::format(L"{}/system.vhd", wsl::windows::common::wslutil::GetMsiPackagePath().value());
#endif

        DiskAttachSettings attachSettings{systemdDistroDiskPath.c_str(), true};
        AttachedDiskInformation attachedDisk;

        VERIFY_SUCCEEDED(AttachDisk((LSWVirtualMachineHandle*)vm, &attachSettings, &attachedDisk));

        MountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", true};
        VERIFY_SUCCEEDED(Mount((LSWVirtualMachineHandle*)vm, &mountSettings));

        std::vector<const char*> cmd = {"/bin/bash", "-c", "echo ok"};
        VERIFY_ARE_EQUAL(RunCommand(vm, cmd), 0);
        LogInfo("Content: %hs", dmesgContent.data());

        auto contentString = std::string(dmesgContent.begin(), dmesgContent.end());

        VERIFY_ARE_NOT_EQUAL(contentString.find("Run /init as init process"), std::string::npos);

        // TODO: stop VM and synchronize
    }

    TEST_METHOD(CreateVmSmokeTest)
    {

        LSWVirtualMachineHandle vm{};
        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        VERIFY_SUCCEEDED(CreateVirualMachine(&settings, (LSWVirtualMachineHandle*)&vm));

#ifdef WSL_SYSTEM_DISTRO_PATH

        std::wstring systemdDistroDiskPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else

        auto systemdDistroDiskPath = std::format(L"{}/system.vhd", wsl::windows::common::wslutil::GetMsiPackagePath().value());
#endif

        DiskAttachSettings attachSettings{systemdDistroDiskPath.c_str(), true};
        AttachedDiskInformation attachedDisk;

        VERIFY_SUCCEEDED(AttachDisk((LSWVirtualMachineHandle*)vm, &attachSettings, &attachedDisk));

        MountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", true};
        VERIFY_SUCCEEDED(Mount((LSWVirtualMachineHandle*)vm, &mountSettings));

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
            VERIFY_SUCCEEDED(CreateLinuxProcess((LSWVirtualMachineHandle*)vm, &createProcessSettings, &pid));

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
            VERIFY_SUCCEEDED(WaitForLinuxProcess((LSWVirtualMachineHandle*)vm, pid, 1000, &result));
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
            VERIFY_SUCCEEDED(CreateLinuxProcess((LSWVirtualMachineHandle*)vm, &createProcessSettings, &pid));

            // Verify that the process is in a running state
            WaitResult result{};
            VERIFY_SUCCEEDED(WaitForLinuxProcess((LSWVirtualMachineHandle*)vm, pid, 1000, &result));
            VERIFY_ARE_EQUAL(result.State, ProcessStateRunning);

            // Verify that it can be killed.
            VERIFY_SUCCEEDED(SignalLinuxProcess((LSWVirtualMachineHandle*)vm, pid, 9));

            // Verify that the process is in a running state

            VERIFY_SUCCEEDED(WaitForLinuxProcess((LSWVirtualMachineHandle*)vm, pid, 1000, &result));
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
            VERIFY_ARE_EQUAL(CreateLinuxProcess((LSWVirtualMachineHandle*)vm, &createProcessSettings, &pid), E_FAIL);

            WaitResult result{};
            VERIFY_ARE_EQUAL(WaitForLinuxProcess((LSWVirtualMachineHandle*)vm, 1234, 1000, &result), E_FAIL);
            VERIFY_ARE_EQUAL(result.State, ProcessStateUnknown);
        }
    }
};