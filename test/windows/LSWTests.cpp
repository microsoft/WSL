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
        wil::com_ptr<ILSWVirtualMachine> vm;

        VIRTUAL_MACHINE_SETTINGS settings{};
        settings.CpuCount = 4;
        settings.DisplayName = L"LSW";
        settings.MemoryMb = 1024;
        settings.BootTimeoutMs = 60 * 1000;
        VERIFY_SUCCEEDED(CreateVm(&settings, &vm));

        VERIFY_SUCCEEDED(vm->GetState());

        system("pause");
    }
};