/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

class WSLCE2EContainerTests
{
    WSL_TEST_CLASS(WSLCE2EContainerTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        // wslc container create --name <containerName>
        auto command = L"container create --name " + WslContainerName;
        WSLCExecutor::ExecuteAndVerify(command, L"", L"Required argument not provided: 'image'", E_INVALIDARG);
    }

    TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        // wslc container create --name <containerName> <invalidImageName>
        auto command = L"container create --name " + WslContainerName + L" " + WslInvalidImageName;
        WSLCExecutor::ExecuteAndVerify(command, L"", L"Image '" + WslInvalidImageName + L"' not found, pulling", WSLA_E_IMAGE_NOT_FOUND);
    }

    TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        return;
        std::wstring containerId{};

        // Create container
        {
            auto command = L"container create --name " + WslContainerName + L" " + WslUbuntuImageName;
            WSLCExecutor::ExecuteAndVerify(command, L"", L"", S_OK);
        }

        // List container
        {
            auto command = L"container list";
            WSLCExecutor::ExecuteAndVerify(command, L"", L"", S_OK);
        }
    }

private:
    const std::wstring WslContainerName = L"wslc-test-container";
    const std::wstring WslInvalidImageName = L"mcr.microsoft.com/invalid-image:latest";
    const std::wstring WslUbuntuImageName = L"ubuntu:latest";
};
} // namespace WSLCE2ETests