/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "VolumeModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::wslc::models;

class WSLCE2EVolumeListTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeListTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        for (const auto& name : FilterTestVolumeNames)
        {
            EnsureVolumeDoesNotExist(name);
        }
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        for (const auto& name : FilterTestVolumeNames)
        {
            EnsureVolumeDoesNotExist(name);
        }
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_HelpCommand)
    {
        auto result = RunWslc(L"volume list --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_InvalidFormatOption)
    {
        auto result = RunWslc(L"volume list --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_QuietOption_OutputsNamesOnly)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"volume list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestVolumeName));
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestVolumeName2));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_JsonFormat)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"volume list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto volumes = ParseNdjsonOutputAs<VolumeListOutput>(result);
        VERIFY_ARE_EQUAL(2U, volumes.size());

        std::vector<std::string> names;
        names.reserve(volumes.size());
        for (const auto& volume : volumes)
        {
            names.push_back(volume.Name);
        }

        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestVolumeName)));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestVolumeName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_ReportsFullFieldSet)
    {
        auto result = RunWslc(std::format(L"volume create --label env=prod {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"volume list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto entries = ParseNdjsonOutput(result);
        VERIFY_ARE_EQUAL(1u, entries.size());
        const auto& volume = entries[0];

        VERIFY_ARE_EQUAL(10u, volume.size());
        VERIFY_ARE_EQUAL("N/A", volume["Availability"].get<std::string>());
        VERIFY_ARE_EQUAL("guest", volume["Driver"].get<std::string>());
        VERIFY_ARE_EQUAL("N/A", volume["Group"].get<std::string>());
        VERIFY_ARE_EQUAL("env=prod", volume["Labels"].get<std::string>());
        VERIFY_ARE_EQUAL("N/A", volume["Links"].get<std::string>());
        VERIFY_IS_FALSE(volume["Mountpoint"].get<std::string>().empty());
        VERIFY_ARE_EQUAL(WideToMultiByte(TestVolumeName), volume["Name"].get<std::string>());
        VERIFY_ARE_EQUAL("local", volume["Scope"].get<std::string>());
        VERIFY_ARE_EQUAL("N/A", volume["Size"].get<std::string>());
        VERIFY_ARE_EQUAL("N/A", volume["Status"].get<std::string>());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_MalformedValue)
    {
        const auto result = RunWslc(L"volume list --filter label");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_InvalidFilterError(L"label")));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_InvalidKey)
    {
        // Filter keys are validated by the Docker daemon, which rejects unknown keys.
        const auto result = RunWslc(L"volume list --filter color=blue");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter 'color'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_InvalidDanglingValue)
    {
        // Dangling values are validated by the Docker daemon, which rejects non-boolean values.
        const auto result = RunWslc(L"volume list --filter dangling=maybe");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter 'dangling=[maybe]'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_Driver)
    {
        const std::wstring alpha = L"wslc-flt-vlist-driver-alpha";
        const std::wstring beta = L"wslc-flt-vlist-driver-beta";
        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(alpha);
            EnsureVolumeDoesNotExist(beta);
        });

        auto result = RunWslc(std::format(L"volume create --driver vhd --opt SizeBytes={} {}", DefaultVolumeSizeBytes, alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create --driver guest {}", beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            const auto names = GetFilteredVolumeNames(L"--filter driver=vhd");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }

        {
            const auto names = GetFilteredVolumeNames(L"--filter driver=guest");
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        // Docker's own driver names (e.g. "local") never match wslc-managed volumes.
        {
            const auto names = GetFilteredVolumeNames(L"--filter driver=local");
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_Label)
    {
        const std::wstring alpha = L"wslc-flt-vlist-label-alpha";
        const std::wstring beta = L"wslc-flt-vlist-label-beta";
        const std::wstring scopeKey = L"wslc.e2e.list_filter_label";
        const std::wstring scopeValue = L"1";

        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(alpha);
            EnsureVolumeDoesNotExist(beta);
        });

        auto result = RunWslc(std::format(L"volume create --label {}={} --label env=prod {}", scopeKey, scopeValue, alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"volume create --label {}={} {}", scopeKey, scopeValue, beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            const auto names = GetFilteredVolumeNames(std::format(L"--filter label={}", scopeKey));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        {
            const auto names = GetFilteredVolumeNames(std::format(L"--filter label={}={} --filter label=env=prod", scopeKey, scopeValue));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }

        {
            const auto names = GetFilteredVolumeNames(std::format(L"--filter label={} --filter label=env=prod", scopeKey));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_JsonEmptyIsExactlyEmpty)
    {
        const std::wstring alpha = L"wslc-flt-vlist-empty-alpha";
        auto cleanup = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(alpha); });

        auto result = RunWslc(std::format(L"volume create {}", alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // NDJSON with zero rows must be exactly empty stdout — not "[]", not "\n".
        result = RunWslc(L"volume list --format json --filter name=wslc-flt-vlist-no-such-volume-zzz");
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_Filter_Name)
    {
        const std::wstring alpha = L"wslc-flt-vlist-name-alpha";
        const std::wstring beta = L"wslc-flt-vlist-name-beta";
        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(alpha);
            EnsureVolumeDoesNotExist(beta);
        });

        auto result = RunWslc(std::format(L"volume create {}", alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            const auto names = GetFilteredVolumeNames(L"--filter name=wslc-flt-vlist-name-");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        {
            const auto names = GetFilteredVolumeNames(L"--filter name=name-alpha");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

private:
    static std::set<std::string> GetFilteredVolumeNames(const std::wstring& filterArgs)
    {
        auto result = RunWslc(std::format(L"volume list --format json {}", filterArgs));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto volumes = ParseNdjsonOutputAs<VolumeListOutput>(result);
        std::set<std::string> names;
        for (const auto& v : volumes)
        {
            names.insert(v.Name);
        }
        return names;
    }

    const std::wstring TestVolumeName = L"wslc-e2e-volume-list";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-list-2";
    const int DefaultVolumeSizeBytes = 3 * 1024 * 1024;
    const std::vector<std::wstring> FilterTestVolumeNames = {
        L"wslc-flt-vlist-driver-alpha",
        L"wslc-flt-vlist-driver-beta",
        L"wslc-flt-vlist-label-alpha",
        L"wslc-flt-vlist-label-beta",
        L"wslc-flt-vlist-empty-alpha",
        L"wslc-flt-vlist-name-alpha",
        L"wslc-flt-vlist-name-beta",
    };
};
} // namespace WSLCE2ETests
