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

    TEST_CLASS_SETUP(TestClassSetup)
    {
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

    TEST_METHOD(CreateVmSmokeTest)
    {
        auto coinit = wil::CoInitializeEx();
        WSADATA Data;
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

        void* vm{};
        VirtualMachineSettings settings{};
        settings.CPU.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.Memory.MemoryMb = 1024;
        settings.Options.BootTimeoutMs = 30000;
        VERIFY_SUCCEEDED(CreateVirualMachine(&settings, (LSWVirtualMachineHandle*)&vm));

        auto systemdDistroDiskPath = LR"(D:\wsldev\system.vhd)";

        DiskAttachSettings attachSettings{systemdDistroDiskPath, false};
        AttachedDiskInformation attachedDisk;
        
        VERIFY_SUCCEEDED(AttachDisk((LSWVirtualMachineHandle*)vm, &attachSettings, &attachedDisk));

        MountSettings mountSettings{attachedDisk.Device, "/mnt", "ext4", "ro", true};
        VERIFY_SUCCEEDED(Mount((LSWVirtualMachineHandle*)vm, &mountSettings));

        std::vector<const char*> commandLine{"/bin/sh", "-c", "echo $foo", nullptr};

        std::vector<ProcessFileDescriptorSettings> fds(3);
        fds[0].Number = 0;
        fds[1].Number = 1;
        fds[2].Number = 2;

        std::vector<const char*> env{"foo=bar", nullptr};
        CreateProcessSettings createProcessSettings{};
        createProcessSettings.Executable = "/bin/sh";
        createProcessSettings.Arguments = commandLine.data();
        createProcessSettings.FileDescriptors = fds.data();
        createProcessSettings.Environment = env.data();
        createProcessSettings.FdCount = 3;

        LinuxProcess process;
        VERIFY_SUCCEEDED(CreateLinuxProcess((LSWVirtualMachineHandle*)vm, &createProcessSettings, &process));

        LogInfo("pid: %lu", process.Pid);

        std::vector<char> buffer(100);

        DWORD bytes{};
        if (!ReadFile(createProcessSettings.FileDescriptors[1].Handle, buffer.data(), (DWORD)buffer.size(), &bytes, nullptr))
        {
            LogError("ReadFile: %lu, handle: 0x%x", GetLastError(), createProcessSettings.FileDescriptors[1].Handle);
            VERIFY_FAIL();
        }

        VERIFY_ARE_EQUAL(buffer.data(), std::string("foo\n"));
        system("pause");
    }
};