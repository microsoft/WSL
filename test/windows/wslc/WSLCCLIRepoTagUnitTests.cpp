/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIRepoTagUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include <ImageModel.h>

using namespace wsl::windows::wslc::models;

namespace WSLCCLIRepoTagUnitTests {

class WSLCCLIRepoTagUnitTests
{
    WSL_TEST_CLASS(WSLCCLIRepoTagUnitTests)

    TEST_METHOD(RepoTag_Parse_ValidInput_ReturnsExpectedRepoAndTag)
    {
        std::vector<std::tuple<std::string, std::string, std::string>> testCases = {
            { "debian", "debian", "" },
            { "debian:latest", "debian", "latest" },
            { "myrepo/debian", "myrepo/debian", "" },
            { "myrepo/debian:latest", "myrepo/debian", "latest" },
            { "myrepo:5000/debian", "myrepo:5000/debian", "" },
            { "myrepo:5000/debian:latest", "myrepo:5000/debian", "latest" }};

        for (const auto& [input, expectedRepo, expectedTag] : testCases)
        {
            auto result = RepoTag::Parse(input);
            VERIFY_ARE_EQUAL(expectedRepo, result.Repo);
            VERIFY_ARE_EQUAL(expectedTag, result.Tag);
        }
    }

    TEST_METHOD(RepoTag_Parse_EdgeCases_ReturnExpectedRepoAndTag)
    {
        std::vector<std::tuple<std::string, std::string, std::string>> testCases = {
            { "", "", "" },
            { "repo:", "repo", "" },
            { ":latest", "", "latest" },
            { "org/team/image", "org/team/image", "" },
            { "registry.example.com:5000/org/team/image", "registry.example.com:5000/org/team/image", "" },
            { "registry.example.com:5000/org/team/image:v1", "registry.example.com:5000/org/team/image", "v1" },
            { "org/team/image:", "org/team/image", "" },
        };

        for (const auto& [input, expectedRepo, expectedTag] : testCases)
        {
            auto result = RepoTag::Parse(input);
            VERIFY_ARE_EQUAL(expectedRepo, result.Repo);
            VERIFY_ARE_EQUAL(expectedTag, result.Tag);
        }
    }
};
} // namespace WSLCCLIRepoTagUnitTests