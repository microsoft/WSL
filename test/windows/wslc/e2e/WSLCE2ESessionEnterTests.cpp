/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionEnterTests.cpp

Abstract:

    This file contains end-to-end tests for the wslc session enter command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "SessionModel.h"

using namespace WEX::Logging;

using wsl::windows::wslc::models::SessionOptions;

namespace WSLCE2ETests {

class WSLCE2ESessionEnterTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionEnterTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        // Ensure that the wslc cli session storage is created.
        RunWslc(L"image ls").Verify({.ExitCode = 0});

        // Terminate the wslc session since we use its storage path in this test class.
        RunWslc(L"session terminate");
        return true;
    }

    TEST_METHOD(WSLCE2E_SessionEnter_WithName)
    {
        WSL2_TEST_ONLY();

        constexpr auto sessionName = L"test-wslc-session-enter";

        // Run an interactive session enter with an explicit name.
        auto session = RunWslcInteractive(std::format(L"session enter \"{}\" --name {}", SessionOptions::GetStoragePath(), sessionName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

        session.ExpectStdout(VT::SESSION_PROMPT);

        // Validate that the shell is running as root.
        session.WriteLine("whoami");
        session.ExpectStdout(VT::RESET);
        session.ExpectCommandEcho("whoami");
        session.ExpectStdout("root\r\n");
        session.ExpectStdout(VT::SESSION_PROMPT);

        // Verify the session appears in session list.
        auto listResult = RunWslc(L"session list");
        listResult.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(listResult.Stdout.has_value());
        VERIFY_IS_TRUE(listResult.Stdout->find(sessionName) != std::wstring::npos);

        // Exit the shell.
        VERIFY_ARE_EQUAL(session.Exit(), 0);

        // Verify the session is no longer in the session list after exiting.
        listResult = RunWslc(L"session list");
        listResult.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(listResult.Stdout.has_value());
        VERIFY_IS_FALSE(listResult.Stdout->find(sessionName) != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_SessionEnter_WithoutName_GeneratesGuid)
    {
        WSL2_TEST_ONLY();

        auto session = RunWslcInteractive(std::format(L"session enter \"{}\"", SessionOptions::GetStoragePath()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

        session.ExpectStderr("Created session: ");
        session.ExpectStdout(VT::SESSION_PROMPT);

        VERIFY_ARE_EQUAL(session.Exit(), 0);
    }

    TEST_METHOD(WSLCE2E_SessionEnter_StoragePathNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"session enter does-not-exist");
        result.Verify({
            .Stderr = L"The system cannot find the path specified. \r\nError code: ERROR_PATH_NOT_FOUND\r\n",
            .ExitCode = 1,
        });
    }
};
} // namespace WSLCE2ETests
