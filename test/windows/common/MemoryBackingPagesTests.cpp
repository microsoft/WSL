// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "IVirtualMachineBackend.h"
#include "hcs.hpp"

namespace MemoryBackingPagesTests {
class MemoryBackingPagesTests
{
    WSL_TEST_CLASS(MemoryBackingPagesTests)

    TEST_METHOD(MemoryPageSize)
    {
        const struct
        {
            std::string_view setting;
            bool largePages;
            bool warning;
        } cases[] = {
            {"", false, false},
            {"memoryPageSize=small", false, false},
            {"memoryPageSize=medium", true, false},
            {"memoryPageSize=large", false, true},
            {"memoryPageSize=invalid", false, true},
            {"memoryPageSize=true", false, true},
            {"memoryPageSize=performance", false, true},
        };

        for (const auto& test : cases)
        {
            LogInfo("Setting: %hs", test.setting.data());
            const auto content = std::format("[experimental]\n{}\n", test.setting);
            auto file = wsl::windows::common::filesystem::TempFile(GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING);
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(file.Handle.get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
            LXSS_ERROR_INFO error{};
            error.Flags = LxssExecutionContextFlagsEnableContextualizedErrors | LxssExecutionContextFlagsEnableUserWarnings;
            {
                wsl::windows::common::ServiceExecutionContext context(&error);
                const wsl::core::Config config(file.Path.c_str());
                VERIFY_ARE_EQUAL(test.largePages, config.EnableLargeMemoryBackingPages);
            }

            wil::unique_cotaskmem_string warnings(error.Warnings);
            VERIFY_ARE_EQUAL(test.warning, warnings && *warnings.get() != L'\0');
            if (test.warning)
            {
                VERIFY_IS_TRUE(std::wstring_view(warnings.get()).find(L"memoryPageSize") != std::wstring_view::npos);
            }
        }
    }

    TEST_METHOD(LargePages)
    {
        VmPlatformCapabilities capabilities{};
        capabilities.Features.set(static_cast<size_t>(VmFeature::SmallPageMemory));
        VmMemoryRequest memory{};
        wsl::windows::common::vm::ConfigureMemoryBackingPages(memory, true, capabilities);
        VERIFY_ARE_EQUAL(VmFeatureRequest::Required, memory.LargePageBacking);
        VERIFY_ARE_EQUAL(VmFeatureRequest::Disabled, memory.SmallPageBacking);
        VERIFY_ARE_EQUAL(9u, memory.PageReportingOrder.value());
        VERIFY_IS_FALSE(memory.FaultClusterSizeShift.has_value());
        VERIFY_IS_FALSE(memory.DirectMapFaultClusterSizeShift.has_value());
    }

    TEST_METHOD(DefaultPages)
    {
        for (const bool smallPagesSupported : {false, true})
        {
            VmPlatformCapabilities capabilities{};
            capabilities.Features.set(static_cast<size_t>(VmFeature::SmallPageMemory), smallPagesSupported);
            VmMemoryRequest memory{};
            wsl::windows::common::vm::ConfigureMemoryBackingPages(memory, false, capabilities);
            VERIFY_ARE_EQUAL(VmFeatureRequest::Disabled, memory.LargePageBacking);
            VERIFY_ARE_EQUAL(smallPagesSupported ? VmFeatureRequest::Required : VmFeatureRequest::Disabled, memory.SmallPageBacking);
            VERIFY_ARE_EQUAL(smallPagesSupported ? 5u : 9u, memory.PageReportingOrder.value());
            VERIFY_ARE_EQUAL(smallPagesSupported, memory.FaultClusterSizeShift.has_value());
            VERIFY_ARE_EQUAL(smallPagesSupported, memory.DirectMapFaultClusterSizeShift.has_value());
            if (smallPagesSupported)
            {
                VERIFY_ARE_EQUAL(4u, memory.FaultClusterSizeShift.value());
                VERIFY_ARE_EQUAL(4u, memory.DirectMapFaultClusterSizeShift.value());
            }
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
