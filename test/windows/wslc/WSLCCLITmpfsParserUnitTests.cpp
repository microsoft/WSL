/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITmpfsParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI tmpfs parsing and validation.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "MountSpecParsing.h"

using namespace wsl::windows::common;

namespace WSLCCLITmpfsParserUnitTests {

class WSLCCLITmpfsParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITmpfsParserUnitTests)

    TEST_METHOD(WSLCCLITmpfsMount_Parse)
    {
        const std::vector<std::tuple<std::wstring, std::string, std::string>> validTmpfsSpecs = {
            {L"", "", ""},
            {L"/tmp", "/tmp", ""},
            {L"/tmp:size=50m", "/tmp", "size=50m"},
            {L"/var/tmp:size=1g", "/var/tmp", "size=1g"},
            {L"/tmp:size=50m,mode=1777", "/tmp", "size=50m,mode=1777"},
            {L"/cache:uid=1000,gid=1000", "/cache", "uid=1000,gid=1000"},
            {L"/mnt/ramdisk:size=256k,nr_inodes=1k", "/mnt/ramdisk", "size=256k,nr_inodes=1k"},
            {L"/securetmp:mode=0700", "/securetmp", "mode=0700"},
            {L"/scratch:nosuid,nodev,noexec", "/scratch", "nosuid,nodev,noexec"},
            {L"/wsl/tmp:size=2g,uid=0,gid=0,mode=1777", "/wsl/tmp", "size=2g,uid=0,gid=0,mode=1777"},
        };

        for (const auto& [input, expectedTarget, expectedOptions] : validTmpfsSpecs)
        {
            const auto result = mount::ParseDockerTmpfsString(input);
            VERIFY_ARE_EQUAL(static_cast<int>(WSLCMountTypeTmpfs), static_cast<int>(result.MountType));
            VERIFY_ARE_EQUAL(expectedTarget, result.Target);
            VERIFY_IS_TRUE(result.TmpfsOptions.has_value());
            VERIFY_ARE_EQUAL(expectedOptions, result.TmpfsOptions.value());
        }
    }

    TEST_METHOD(WSLCCLITmpfsMount_Validate)
    {
        auto valid = mount::ParseDockerTmpfsString(L"/tmp:size=50m");
        VERIFY_NO_THROW(mount::ValidateMountSpec(valid));

        auto empty = mount::ParseDockerTmpfsString(L":size=50m");
        VERIFY_THROWS(mount::ValidateMountSpec(empty), mount::MountValidationException);

        auto relative = mount::ParseDockerTmpfsString(L"tmp:size=50m");
        VERIFY_THROWS(mount::ValidateMountSpec(relative), mount::MountValidationException);
    }
};

} // namespace WSLCCLITmpfsParserUnitTests
