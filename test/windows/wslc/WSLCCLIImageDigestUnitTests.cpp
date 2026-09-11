/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIImageDigestUnitTests.cpp

Abstract:

    This file contains unit tests for image digest formatting.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "ImageModel.h"

using namespace wsl::windows::wslc::models;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIImageDigestUnitTests {
class WSLCCLIImageDigestUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIImageDigestUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        Log::Comment(L"WSLC CLI Image Digest Unit Tests - Class Setup");
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        Log::Comment(L"WSLC CLI Image Digest Unit Tests - Class Cleanup");
        return true;
    }

    // Test: A repo digest is reduced to the digest that is displayed.
    TEST_METHOD(DigestFromRepoDigest_StripsRepositoryPrefix)
    {
        constexpr std::string_view digest = "sha256:28bd5fe8b56d1bd048e5babf5b10710ebe0bae67db86916198a6eec434943f8b";

        VERIFY_ARE_EQUAL(digest, DigestFromRepoDigest(std::string{"alpine@"} + std::string{digest}));
        VERIFY_ARE_EQUAL(digest, DigestFromRepoDigest(std::string{"docker.io/library/alpine@"} + std::string{digest}));

        // Registries may carry a port, which contains no '@' and must not confuse the split.
        VERIFY_ARE_EQUAL(digest, DigestFromRepoDigest(std::string{"localhost:5000/team/app@"} + std::string{digest}));
    }

    // Test: Values without a separator are passed through untouched.
    TEST_METHOD(DigestFromRepoDigest_PassesThroughBareValues)
    {
        VERIFY_ARE_EQUAL(std::string_view{""}, DigestFromRepoDigest(""));
        VERIFY_ARE_EQUAL(std::string_view{"sha256:abc"}, DigestFromRepoDigest("sha256:abc"));
        VERIFY_ARE_EQUAL(std::string_view{"<none>"}, DigestFromRepoDigest("<none>"));
    }

    // Test: Only the first separator splits, so a digest is never truncated further.
    TEST_METHOD(DigestFromRepoDigest_SplitsOnFirstSeparator)
    {
        VERIFY_ARE_EQUAL(std::string_view{"sha256:abc@def"}, DigestFromRepoDigest("repo@sha256:abc@def"));
    }
};
} // namespace WSLCCLIImageDigestUnitTests
