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
        VERIFY_ARE_EQUAL(
            static_cast<size_t>(1), result.GetStdoutLines().size(), L"Expected only the header row when there are no containers");
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_MultipleContainers_CompletesInConstantTime)
    {
        // Verify that stats fetches containers in parallel rather than sequentially.
        //
        // The Docker engine blocks for ~1s per stats request (without one-shot=true) to
        // collect a valid CPU sample. Sequential fetching therefore takes AT LEAST
        // (c_containerCount * c_dockerSampleMinMs) wall time - even on the fastest machine.
        // Parallel fetching takes approximately one sample period regardless of count.
        //
        // Two independent constants keep the thresholds honest:
        //   c_dockerSampleMinMs  - the Docker engine's inherent minimum blocking time per call.
        //                          This is a property of the API, not the machine speed.
        //   c_parallelMaxMs      - generous wall-time budget for the parallel fetch on a
        //                          stressed CI machine. Must be < c_sequentialMinMs so that
        //                          a regression to sequential always fires the assertion.
        constexpr size_t c_containerCount = 20;
        constexpr DWORD c_dockerSampleMinMs = 900; // Docker always blocks at least ~1s per stats call, give some leeway for timing variance.
        constexpr DWORD c_sequentialMinMs = c_dockerSampleMinMs * static_cast<DWORD>(c_containerCount); // true floor for sequential
        constexpr DWORD c_parallelMaxMs = c_sequentialMinMs - 5000; // generous parallel budget, clearly below sequential floor
        constexpr DWORD c_minExpectedMs = 500;                      // sampling delay must be occurring

        // The parallel ceiling must be clearly below the sequential floor for the test to be meaningful.
        static_assert(c_parallelMaxMs < c_sequentialMinMs, "Timing thresholds would not catch a sequential regression");

        // Register cleanup before creating any containers so that containers created in a
        // partial run are still removed if a subsequent creation or verification step throws.
        std::vector<std::wstring> containerNames;
        auto cleanup = wil::scope_exit([&]() {
            for (const auto& name : containerNames)
            {
                EnsureContainerDoesNotExist(name);
            }
        });

        for (size_t i = 0; i < c_containerCount; ++i)
        {
            const auto name = std::format(L"{}-{}", WslcContainerName, i);
            containerNames.push_back(name);
            auto runResult = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", name, DebianImage.NameAndTag()));
            runResult.Verify({.Stderr = L"", .ExitCode = 0});
        }

        const auto startTime = std::chrono::steady_clock::now();
        auto result = RunWslc(L"container stats");
        const auto elapsedMs = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());

        result.Verify({.Stderr = L"", .ExitCode = 0});

        // All containers must appear in the output.
        for (const auto& name : containerNames)
        {
            VERIFY_IS_TRUE(result.StdoutContainsSubstring(name), std::format(L"Container '{}' not found in stats output", name).c_str());
        }

        WEX::Logging::Log::Comment(
            std::format(L"Stats for {} containers completed in {}ms (parallel max: {}ms, sequential min: {}ms)", c_containerCount, elapsedMs, c_parallelMaxMs, c_sequentialMinMs)
                .c_str());

        VERIFY_IS_GREATER_THAN_OR_EQUAL(
            elapsedMs, c_minExpectedMs, L"Stats completed suspiciously fast - Docker sampling may not be occurring");
        VERIFY_IS_LESS_THAN(
            elapsedMs, c_parallelMaxMs, L"Stats exceeded parallel time budget - fetching may have regressed to sequential");
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Stats_JsonFormat)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Get stats in JSON format
        result = RunWslc(std::format(L"container stats --no-trunc --format json {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Parse and validate the JSON output
        const auto json = nlohmann::json::parse(WideToMultiByte(result.Stdout.value()));
        VERIFY_IS_TRUE(json.is_array());
        VERIFY_IS_GREATER_THAN_OR_EQUAL(json.size(), 1U);

        const auto& entry = json[0];
        VERIFY_IS_TRUE(entry.contains("ID"));
        VERIFY_IS_TRUE(entry.contains("Name"));
        VERIFY_IS_TRUE(entry.contains("CPUPerc"));
        VERIFY_IS_TRUE(entry.contains("MemUsage"));
        VERIFY_IS_TRUE(entry.contains("MemPerc"));
        VERIFY_IS_TRUE(entry.contains("NetIO"));
        VERIFY_IS_TRUE(entry.contains("BlockIO"));
        VERIFY_IS_TRUE(entry.contains("PIDs"));

        VERIFY_ARE_EQUAL(containerId, MultiByteToWide(entry["ID"].get<std::string>()));
        VERIFY_IS_TRUE(entry["CPUPerc"].is_string());
        VERIFY_IS_TRUE(entry["MemUsage"].is_string());
        VERIFY_IS_TRUE(entry["MemPerc"].is_string());
        VERIFY_IS_TRUE(entry["NetIO"].is_string());
        VERIFY_IS_TRUE(entry["BlockIO"].is_string());
        VERIFY_IS_TRUE(entry["PIDs"].is_number_unsigned());
    }

private:
    const std::wstring WslcContainerName = L"wslc-stats-test";
    const TestImage& DebianImage = DebianTestImage();
};

} // namespace WSLCE2ETests
