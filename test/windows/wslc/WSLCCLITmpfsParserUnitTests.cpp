/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITmpfsParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI tmpfs parsing and validation.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ContainerModel.h"

using namespace wsl::windows::wslc;

namespace WSLCCLITmpfsParserUnitTests {

class WSLCCLITmpfsParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITmpfsParserUnitTests)

    TEST_METHOD(WSLCCLITmpfsMount_Parse)
    {
        std::vector<std::tuple<std::string, std::string, std::string>> validTmpfsSpecs = {
            {"/tmp", "/tmp", ""},
            {"/tmp:size=50m", "/tmp", "size=50m"},
            {"/var/tmp:size=1g", "/var/tmp", "size=1g"},
            {"/tmp:size=50m,mode=1777", "/tmp", "size=50m,mode=1777"},
            {"/cache:uid=1000,gid=1000", "/cache", "uid=1000,gid=1000"},
            {"/mnt/ramdisk:size=256k,nr_inodes=1k", "/mnt/ramdisk", "size=256k,nr_inodes=1k"},
            {"/securetmp:mode=0700", "/securetmp", "mode=0700"},
            {"/scratch:nosuid,nodev,noexec", "/scratch", "nosuid,nodev,noexec"},
            {"/wsl/tmp:size=2g,uid=0,gid=0,mode=1777", "/wsl/tmp", "size=2g,uid=0,gid=0,mode=1777"},
        };
    }
};

} // namespace WSLCCLITmpfsParserUnitTests