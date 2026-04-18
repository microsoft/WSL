/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVersionTests.cpp

Abstract:

    This file contains end-to-end tests for the wslc version command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "VersionModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "Argument.h"
#include "JsonUtils.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

using namespace wsl::windows::wslc::models;

class WSLCE2EVersionTests
{
    WSLC_TEST_CLASS(WSLCE2EVersionTests)

    WSLC_TEST_METHOD(WSLCE2E_Version_HelpCommand)
    {
        auto result = RunWslc(L"version --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Version_DefaultFormatIsTable)
    {
        const auto result = RunWslc(L"version");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stdout->find(L"Client:"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stdout->find(L"Server:"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Version_TableFormatMatchesDefault)
    {
        const auto defaultResult = RunWslc(L"version");
        defaultResult.Verify({.Stderr = L"", .ExitCode = 0});

        const auto explicitResult = RunWslc(L"version --format table");
        explicitResult.Verify({.Stdout = defaultResult.Stdout, .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Version_JsonFormat)
    {
        const auto result = RunWslc(L"version --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());

        const auto versionInfoFromJson = wsl::shared::FromJson<VersionInfo>(result.Stdout->c_str());
        VERIFY_ARE_EQUAL(VersionInfo{}.Client.Version, versionInfoFromJson.Client.Version);
    }

    WSLC_TEST_METHOD(WSLCE2E_Version_InvalidFormatOption)
    {
        const auto result = RunWslc(L"version --format yaml");
        result.Verify({.Stderr = L"Invalid format value: yaml is not a recognized format type. Supported format types are: json, table.\r\n", .ExitCode = 1});
    }

private:
    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()  //
               << GetDescription() //
               << GetUsage()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_VersionLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc version [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  --format   Output formatting (json or table) (Default: table)\r\n"
                << L"  -h,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
