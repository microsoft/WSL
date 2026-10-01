/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTestBase.cpp

Abstract:

    Shared fixture state for the WSLC API test classes.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

WSLCTestFixture& WSLCTestFixture::Instance()
{
    static WSLCTestFixture fixture;

    return fixture;
}

void WSLCTestFixtureCleanup()
{
    auto& fixture = WSLCTestFixture::Instance();
    if (!fixture.Initialized)
    {
        return;
    }

    fixture.DefaultSession.reset();

    // Keep the VHD when running in -f mode, to speed up subsequent test runs.
    if (!g_fastTestRun && !fixture.StoragePath.empty())
    {
        std::error_code error;
        std::filesystem::remove_all(fixture.StoragePath, error);
        if (error)
        {
            LogError("Failed to cleanup storage path %ws: %hs", fixture.StoragePath.c_str(), error.message().c_str());
        }
    }

    WSACleanup();

    fixture.Initialized = false;
}
