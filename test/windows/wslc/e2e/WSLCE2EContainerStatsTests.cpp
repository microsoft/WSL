// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    WSLCE2EContainerStatsTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC container stats command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using namespace wsl::shared;
using namespace wsl::windows::common::string;

class WSLCE2EContainerStatsTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerStatsTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_HelpCommand)
    {
        auto result = RunWslc(L"container stats --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_NoContainers)
    {
        // With no running containers, stats should produce no output rows (header only or empty).
        auto result = RunWslc(L"container stats");
        result.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_RunningContainer_HasExpectedColumns)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto result = RunWslc(L"container stats");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto lines = result.GetStdoutLines();
        VERIFY_IS_TRUE(lines.size() >= 2, L"Expected header row and at least one data row");

        // Verify the header row contains all expected column titles.
        const auto& header = lines[0];
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderContainerId()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderName()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderCpuPercent()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderMemUsageLimit()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderMemPercent()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderNetIo()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderBlockIo()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, header.find(Localization::WSLCCLI_TableHeaderPids()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_RunningContainer_ContainerIdAndNamePresent)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(runResult.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        auto result = RunWslc(L"container stats");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // The data row must contain the truncated container ID and the container name.
        bool foundContainer = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                foundContainer = true;
                VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(WslcContainerName));
                break;
            }
        }

        VERIFY_IS_TRUE(foundContainer, L"Container ID not found in stats output");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_RunningContainer_NoTrunc)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto fullContainerId = runResult.GetStdoutOneLine();
        VERIFY_IS_FALSE(fullContainerId.empty());

        auto result = RunWslc(L"container stats --no-trunc");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // With --no-trunc the full container ID must appear.
        bool foundContainer = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(fullContainerId) != std::wstring::npos)
            {
                foundContainer = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundContainer, L"Full container ID not found in stats --no-trunc output");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_SpecificContainerId)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(runResult.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Pass the container ID explicitly — only that container's row should appear.
        auto result = RunWslc(std::format(L"container stats {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto lines = result.GetStdoutLines();
        // Header + exactly one data row.
        VERIFY_ARE_EQUAL(2u, lines.size());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, lines[1].find(containerId));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_StoppedContainerExcluded)
    {
        // Create (but do not start) a container.
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(createResult.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Stats without --all must not show non-running containers.
        auto result = RunWslc(L"container stats");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        for (const auto& line : result.GetStdoutLines())
        {
            VERIFY_ARE_EQUAL(std::wstring::npos, line.find(containerId));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_AllFlag_IncludesStoppedContainers)
    {
        // Create (but do not start) a container.
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(createResult.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Stats with --all must include non-running containers.
        auto result = RunWslc(L"container stats --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool foundContainer = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                foundContainer = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundContainer, L"Stopped container not found in stats --all output");
    }

private:
    const std::wstring WslcContainerName = L"wslc-stats-test";
    const TestImage& DebianImage = DebianTestImage();
};

} // namespace WSLCE2ETests
