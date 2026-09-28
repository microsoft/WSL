/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESettingsTests.cpp

Abstract:

    End-to-end tests for the wslc `settings` command tree. Tests mutate the
    user's real settings file at %LOCALAPPDATA%\wslc\settings.yaml; HostFileChange
    backs the file up on construction and restores it on scope exit.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ESettingsTests
{
    WSLC_TEST_CLASS(WSLCE2ESettingsTests)

    WSLC_TEST_METHOD(WSLCE2E_Settings_Reset_RewritesFile)
    {
        const auto settingsPath = GetSettingsPath();

        HostFileChange settings(settingsPath, "session:\n  cpuCount: 2\n");

        auto result = RunWslc(L"settings reset");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto content = ReadFileContent(settingsPath.wstring());
        VERIFY_ARE_EQUAL(std::wstring::npos, content.find(L"cpuCount: 2"));
    }

private:
    static std::filesystem::path GetSettingsPath()
    {
        return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"settings.yaml";
    }
};

} // namespace WSLCE2ETests
