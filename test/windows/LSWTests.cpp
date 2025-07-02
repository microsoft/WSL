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

        wil::com_ptr<ILSWVirtualMachine> vm;

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.MemoryMb = 1024;
        settings.BootTimeoutMs = 60 * 1000;
        VERIFY_SUCCEEDED(CreateVm(&settings, &vm));

        VERIFY_SUCCEEDED(vm->GetState());

        auto systemdDistroDiskPath = LR"(D:\wsldev\system.vhd)";

        wil::unique_cotaskmem_ansistring device;
        VERIFY_SUCCEEDED(vm->AttachDisk(systemdDistroDiskPath, false, &device));

        VERIFY_SUCCEEDED(vm->Mount(device.get(), L"/mnt", L"ext4", L"ro"));

        std::vector<const char*> commandLine{"/bin/sh", "-c", "echo foo"};
        LSW_CREATE_PROCESS_OPTIONS options{};
        options.Executable = "/bin/sh";
        options.CommandLineCount = 3;
        options.CommandLine = commandLine.data();
        options.EnvironmnentCount = 0;

        LSW_CREATE_PROCESS_RESULT result;

        VERIFY_SUCCEEDED(vm->CreateLinuxProcess(&options, &result));

        VERIFY_ARE_EQUAL(result.Errno, 0);

        std::vector<char> buffer(100);

        DWORD bytes{};
        if (!ReadFile(result.Fds[1].Handle, buffer.data(), (DWORD)buffer.size(), &bytes, nullptr))
        {
            LogError("ReadFile: %lu, handle: 0x%x", GetLastError(), result.Fds[1].Handle);
            VERIFY_FAIL();
        }

        VERIFY_ARE_EQUAL(buffer.data(), std::string("foo\n"));
        system("pause");
    }
};