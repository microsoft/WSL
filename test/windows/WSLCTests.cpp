/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTests.cpp

Abstract:

    This file contains test cases for WSLC (WSL Container CLI).

--*/

#include "precomp.h"
#include "Common.h"
#include <filesystem>

namespace WSLCTests {
class WSLCTests
{
    WSL_TEST_CLASS(WSLCTests)

    // Initialize the tests
    TEST_CLASS_SETUP(TestClassSetup)
    {
        ////VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        return true;
    }

    // Uninitialize the tests
    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        ////LxsstuUninitialize(FALSE);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        ////LxssLogKernelOutput();
        return true;
    }

    // Test basic WSLC version command
    TEST_METHOD(LaunchTest)
    {
        /*
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { DeleteFile(L"output.txt"); });

        std::ofstream file("output.txt");
        VERIFY_IS_TRUE(file.good() && file << "previous content\n");
        file.close();


        std::wstring cmd(L"C:\\windows\\system32\\cmd.exe /c \"wslc2.exe\"");
        auto [output, error] = LxsstuLaunchCommandAndCaptureOutput(cmd.data());
        
        ///VERIFY_IS_TRUE(output.find(L"Windows Subsystem for Linux Container CLI") != std::wstring::npos);
        VERIFY_ARE_EQUAL(error, L"");
        */
    }
};
} // namespace WSLCTests