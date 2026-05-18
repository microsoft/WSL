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

    WSLCExecutionResult RunContainerExe(const std::wstring& commandLine)
    {
        auto cmd = L"\"" + GetContainerExePath() + L"\" " + commandLine;
        wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
        const auto output = process.RunAndCaptureOutput();
        return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = output.ExitCode};
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

    // Verify that container.exe responds to --help identically to wslc.exe,
    // confirming it is a functional alias and not just a placeholder.
    WSLC_TEST_METHOD(WSLCE2E_ContainerExe_HelpCommand_MatchesWslc)
    {
        const auto wslcResult = RunWslc(L"--help");
        wslcResult.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containerResult = RunContainerExe(L"--help");
        containerResult.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_ARE_EQUAL(wslcResult.Stdout.value(), containerResult.Stdout.value());
    }
};

} // namespace WSLCE2ETests
