/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EAliasTests.cpp

Abstract:

    This file contains end-to-end tests for verifying the command alias functionality.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

namespace {

    inline std::wstring GetContainerExePath()
    {
        return (std::filesystem::path(wsl::windows::common::wslutil::GetMsiPackagePath().value()) / L"container.exe").wstring();
    }

} // namespace

class WSLCE2EAliasTests
{
    WSLC_TEST_CLASS(WSLCE2EAliasTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Verify that container.exe exists on disk as the deployed alias for wslc.exe.
    WSLC_TEST_METHOD(WSLCE2E_ContainerExe_IsDeployed)
    {
        const auto containerExePath = GetContainerExePath();
        VERIFY_IS_TRUE(std::filesystem::exists(containerExePath), (L"container.exe not found at: " + containerExePath).c_str());
    }
};

} // namespace WSLCE2ETests
