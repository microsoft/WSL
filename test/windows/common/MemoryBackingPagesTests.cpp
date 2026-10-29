// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "hcs.hpp"
#include "WslcsdkPrivate.h"

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
        using wsl::windows::common::helpers::WindowsBuildNumbers;
        const bool smallPagesSupported =
            (windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium) ||
            (windowsVersion.BuildNumber >= WindowsBuildNumbers::Cobalt && windowsVersion.UpdateBuildRevision >= 2360) ||
            (windowsVersion.BuildNumber >= WindowsBuildNumbers::Iron && windowsVersion.UpdateBuildRevision >= 1970) ||
            (windowsVersion.BuildNumber >= WindowsBuildNumbers::Vibranium_22H2 && windowsVersion.UpdateBuildRevision >= 3393);

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

    TEST_METHOD(RuntimeVersionRequirement)
    {
        WSLCCompatVersion oldVersion{3, 0, 9};
        if (WSL_PACKAGE_VERSION_MAJOR == oldVersion.Major && WSL_PACKAGE_VERSION_MINOR == oldVersion.Minor &&
            WSL_PACKAGE_VERSION_REVISION == oldVersion.Revision)
        {
            --oldVersion.Revision;
        }

        wil::unique_cotaskmem_string message;
        VERIFY_ARE_EQUAL(WSLC_E_RUNTIME_UPDATE_NEEDED, CheckWslRuntimeVersionSupport(oldVersion, {3, 1, 0}, &message));
        VERIFY_IS_NOT_NULL(message.get());
        VERIFY_ARE_EQUAL(
            wsl::shared::Localization::MessageWslcOperationRequiresWslVersion(
                std::format("{}.{}.{}", oldVersion.Major, oldVersion.Minor, oldVersion.Revision), "3.1.0"),
            std::wstring(message.get()));
        VERIFY_ARE_EQUAL(WSLC_E_RUNTIME_UPDATE_NEEDED, CheckWslRuntimeVersionSupport(oldVersion, {3, 1, 0}, nullptr));

        for (const WSLCCompatVersion version : {WSLCCompatVersion{3, 1, 0}, WSLCCompatVersion{3, 1, 1}, WSLCCompatVersion{4, 0, 0}})
        {
            message.reset();
            VERIFY_SUCCEEDED(CheckWslRuntimeVersionSupport(version, {3, 1, 0}, &message));
            VERIFY_IS_NULL(message.get());
        }
    }

    TEST_METHOD(MatchingDevelopmentRuntime)
    {
        const WSLCCompatVersion version{WSL_PACKAGE_VERSION_MAJOR, WSL_PACKAGE_VERSION_MINOR, WSL_PACKAGE_VERSION_REVISION};
        wil::unique_cotaskmem_string message;
        VERIFY_SUCCEEDED(CheckWslRuntimeVersionSupport(version, {UINT32_MAX, UINT32_MAX, UINT32_MAX}, &message));
        VERIFY_IS_NULL(message.get());
    }
};
} // namespace MemoryBackingPagesTests
