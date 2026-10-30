// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "hcs.hpp"

namespace MemoryBackingPagesTests {
class MemoryBackingPagesTests
{
    WSL_TEST_CLASS(MemoryBackingPagesTests)

    TEST_METHOD(LargePages)
    {
        wsl::windows::common::hcs::Memory memory{};
        VERIFY_ARE_EQUAL(9, wsl::windows::common::hcs::ConfigureMemoryBackingPages(memory, true));

        const nlohmann::json settings = memory;
        VERIFY_ARE_EQUAL(std::string("Large"), settings.at("BackingPageSize").get<std::string>());
        VERIFY_IS_FALSE(settings.contains("FaultClusterSizeShift"));
        VERIFY_IS_FALSE(settings.contains("DirectMapFaultClusterSizeShift"));
    }

    TEST_METHOD(DefaultPages)
    {
        const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
        const bool smallPagesSupported =
            wsl::windows::common::hcs::IsSmallMemoryBackingPagesSupported(windowsVersion.BuildNumber, windowsVersion.UpdateBuildRevision);

        wsl::windows::common::hcs::Memory memory{};
        VERIFY_ARE_EQUAL(smallPagesSupported ? 5 : 9, wsl::windows::common::hcs::ConfigureMemoryBackingPages(memory, false));

        const nlohmann::json settings = memory;
        if (smallPagesSupported)
        {
            VERIFY_ARE_EQUAL(std::string("Small"), settings.at("BackingPageSize").get<std::string>());
            VERIFY_ARE_EQUAL(4u, settings.at("FaultClusterSizeShift").get<uint32_t>());
            VERIFY_ARE_EQUAL(4u, settings.at("DirectMapFaultClusterSizeShift").get<uint32_t>());
        }
        else
        {
            VERIFY_IS_FALSE(settings.contains("BackingPageSize"));
            VERIFY_IS_FALSE(settings.contains("FaultClusterSizeShift"));
            VERIFY_IS_FALSE(settings.contains("DirectMapFaultClusterSizeShift"));
        }
    }

    TEST_METHOD(SmallPageSupportBoundaries)
    {
        const struct
        {
            ULONG buildNumber;
            DWORD updateBuildRevision;
            bool supported;
        } cases[] = {
            {19044, MAXDWORD, false},
            {19045, 3392, false},
            {19045, 3393, true},
            {20348, 1969, false},
            {20348, 1970, true},
            {22000, 1969, false},
            {22000, 1970, false},
            {22000, 2000, false},
            {22000, 2359, false},
            {22000, 2360, true},
            {22621, 1970, false},
            {22621, 2360, true},
            {25398, 0, false},
            {25398, 2360, true},
            {26099, 2359, false},
            {26099, 2360, true},
            {26100, 0, true},
            {26101, 0, true},
        };

        for (const auto& test : cases)
        {
            LogInfo("Windows build %lu.%lu", test.buildNumber, test.updateBuildRevision);
            VERIFY_ARE_EQUAL(test.supported, wsl::windows::common::hcs::IsSmallMemoryBackingPagesSupported(test.buildNumber, test.updateBuildRevision));
        }
    }
};
} // namespace MemoryBackingPagesTests
