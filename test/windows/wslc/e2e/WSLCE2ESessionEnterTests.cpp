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
#include "WSLCSessionDefaults.h"
#include "WSLCUserSettings.h"

using namespace WEX::Logging;

namespace WSLCE2ETests {

namespace {

    const std::filesystem::path& GetDefaultStoragePath()
    {
        auto isElevated = wsl::windows::common::security::IsTokenElevated(wil::open_current_access_token(TOKEN_QUERY).get());

        const auto& userSettings = wsl::windows::wslc::settings::User();
        auto customPath = userSettings.Get<wsl::windows::wslc::settings::Setting::SessionStoragePath>();

        static const std::filesystem::path basePath =
            customPath.empty() ? (wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / wsl::windows::wslc::DefaultStorageSubPath)
                               : std::filesystem::path{customPath};

        // Session names are now qualified with the username (e.g. "wslc-cli-alice").
        wchar_t username[256 + 1] = {};
        DWORD usernameLen = ARRAYSIZE(username);
        THROW_IF_WIN32_BOOL_FALSE(GetUserNameW(username, &usernameLen));

        auto adminName = std::format(L"{}-{}", wsl::windows::wslc::DefaultAdminSessionName, username);
        auto nonAdminName = std::format(L"{}-{}", wsl::windows::wslc::DefaultSessionName, username);

        static const std::filesystem::path storagePathNonAdmin = basePath / nonAdminName;
        static const std::filesystem::path storagePathAdmin = basePath / adminName;

        return isElevated ? storagePathAdmin : storagePathNonAdmin;
    }

} // namespace

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

    WSLC_TEST_METHOD(WSLCE2E_SessionEnter_WithName)
    {
        constexpr auto sessionName = L"test-wslc-session-enter";

        // Run an interactive session enter with an explicit name.
        auto session = RunWslcInteractive(std::format(L"session enter \"{}\" --name {}", GetDefaultStoragePath(), sessionName));
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

    WSLC_TEST_METHOD(WSLCE2E_SessionEnter_WithoutName_GeneratesGuid)
    {
        auto session = RunWslcInteractive(std::format(L"session enter \"{}\"", GetDefaultStoragePath()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

        session.ExpectStderr("Created session: ");
        session.ExpectStdout(VT::SESSION_PROMPT);

        VERIFY_ARE_EQUAL(session.Exit(), 0);
    }

    WSLC_TEST_METHOD(WSLCE2E_SessionEnter_StoragePathNotFound)
    {
        auto result = RunWslc(L"session enter does-not-exist");
        result.Verify({
            .Stderr = L"The system cannot find the path specified. \r\nError code: ERROR_PATH_NOT_FOUND\r\n",
            .ExitCode = 1,
        });
    }
};
} // namespace WSLCE2ETests
