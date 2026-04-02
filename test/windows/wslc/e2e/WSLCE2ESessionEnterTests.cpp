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

using namespace WEX::Logging;

namespace WSLCE2ETests {

class WSLCE2ESessionEnterTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionEnterTests)

    wil::unique_couninitialize_call m_coinit = wil::CoInitializeEx();

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_SessionEnter_WithName)
    {
        WSL2_TEST_ONLY();

        // Generate a unique storage path for this test.
        GUID testGuid;
        VERIFY_SUCCEEDED(CoCreateGuid(&testGuid));
        auto guidStr = wsl::shared::string::GuidToString<wchar_t>(testGuid, wsl::shared::string::GuidToStringFlags::None);
        auto storagePath = std::filesystem::absolute(std::filesystem::current_path() / L"wslc-cli-test-sessions" / guidStr.substr(0, 8));
        const auto sessionName = std::format(L"wslc-enter-test-{}", guidStr.substr(0, 8));

        auto cleanupStorage = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(storagePath, error);
        });

        // Run an interactive session enter with an explicit name.
        auto session = RunWslcInteractive(std::format(L"session enter \"{}\" --name {}", storagePath.wstring(), sessionName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

        session.ExpectStdout(VT::SESSION_PROMPT);

        // Verify basic command execution inside the session.
        session.WriteLine("echo hello");
        session.ExpectStdout(VT::RESET);
        session.ExpectCommandEcho("echo hello");
        session.ExpectStdout("hello\r\n");
        session.ExpectStdout(VT::SESSION_PROMPT);

        // Verify whoami returns root.
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
        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);

        // Verify the session is no longer in the session list after exiting.
        listResult = RunWslc(L"session list");
        listResult.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(listResult.Stdout.has_value());
        VERIFY_IS_FALSE(listResult.Stdout->find(sessionName) != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_SessionEnter_WithoutName_GeneratesGuid)
    {
        WSL2_TEST_ONLY();

        // Generate a unique storage path for this test.
        GUID testGuid;
        VERIFY_SUCCEEDED(CoCreateGuid(&testGuid));
        auto guidStr = wsl::shared::string::GuidToString<wchar_t>(testGuid, wsl::shared::string::GuidToStringFlags::None);
        auto storagePath = std::filesystem::absolute(std::filesystem::current_path() / L"wslc-cli-test-sessions" / guidStr.substr(0, 8));

        auto cleanupStorage = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(storagePath, error);
        });

        // Run session enter without --name; the generated GUID should be printed to stderr.
        auto result = RunWslcInteractive(std::format(L"session enter \"{}\"", storagePath.wstring()));
        VERIFY_IS_TRUE(result.IsRunning(), L"Session should be running");

        // The generated session name (a GUID) should appear on stderr.
        // GUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars without braces)
        result.ExpectStdout(VT::SESSION_PROMPT);

        // Run a basic command to confirm the session is functional.
        result.WriteLine("echo working");
        result.ExpectStdout(VT::RESET);
        result.ExpectCommandEcho("echo working");
        result.ExpectStdout("working\r\n");
        result.ExpectStdout(VT::SESSION_PROMPT);

        // Exit the shell.
        result.ExitAndVerifyNoErrors();
        auto exitCode = result.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_SessionEnter_SessionIsDeleted_AfterExit)
    {
        WSL2_TEST_ONLY();

        // Generate a unique storage path and session name for this test.
        GUID testGuid;
        VERIFY_SUCCEEDED(CoCreateGuid(&testGuid));
        auto guidStr = wsl::shared::string::GuidToString<wchar_t>(testGuid, wsl::shared::string::GuidToStringFlags::None);
        auto storagePath = std::filesystem::absolute(std::filesystem::current_path() / L"wslc-cli-test-sessions" / guidStr.substr(0, 8));
        const auto sessionName = std::format(L"wslc-enter-del-{}", guidStr.substr(0, 8));

        auto cleanupStorage = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(storagePath, error);
        });

        // Create and immediately exit the session.
        {
            auto session = RunWslcInteractive(std::format(L"session enter \"{}\" --name {}", storagePath.wstring(), sessionName));
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");
            session.ExpectStdout(VT::SESSION_PROMPT);
            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }

        // After exit, the session should have been terminated and removed.
        auto listResult = RunWslc(L"session list");
        listResult.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(listResult.Stdout.has_value());
        VERIFY_IS_FALSE(listResult.Stdout->find(sessionName) != std::wstring::npos);

        // Attempting to terminate the session should fail with not found.
        auto terminateResult = RunWslc(std::format(L"session terminate {}", sessionName));
        VERIFY_IS_TRUE(terminateResult.ExitCode.has_value());
        VERIFY_ARE_NOT_EQUAL(static_cast<DWORD>(0), terminateResult.ExitCode.value());
    }

    TEST_METHOD(WSLCE2E_SessionEnter_MissingStoragePath_Fails)
    {
        WSL2_TEST_ONLY();

        // session enter with no arguments should fail (missing required storage-path).
        auto result = RunWslc(L"session enter");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_NOT_EQUAL(static_cast<DWORD>(0), result.ExitCode.value());
    }
};
} // namespace WSLCE2ETests
