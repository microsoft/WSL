/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerPruneTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container prune command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerPruneTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerPruneTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);

        // Clean up any leftover containers from previous failed runs
        EnsureContainerDoesNotExist(L"prune-test-container");
        EnsureContainerDoesNotExist(L"prune-running-test");
        EnsureContainerDoesNotExist(L"prune-multi-1");
        EnsureContainerDoesNotExist(L"prune-multi-2");
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        // Clean up any leftover containers
        RunWslc(L"container prune");
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"container prune --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_NoStoppedContainers)
    {
        // Establish a clean baseline first so this does not depend on what other tests left behind.
        RunWslc(L"container prune").Verify({.Stderr = L"", .ExitCode = 0});

        const auto result = RunWslc(L"container prune");

        // With nothing pruned docker emits only the reclaimed-space line, with no header or leading blank line.
        result.Verify({.Stdout = L"Total reclaimed space: 0B\r\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_StoppedContainer)
    {
        // Create and stop a container, then prune it
        auto createResult = RunWslc(std::format(L"container create --name prune-test-container {}", DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = createResult.GetStdoutOneLine();

        auto cleanup = wil::scope_exit([&]() { RunWslc(L"container prune"); });

        // The created container is in stopped state, so prune should remove it
        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify pruned container ID is in output
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(containerId));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_ContainerPruneDeletedHeader()));

        // Verify the container is actually removed
        VerifyContainerIsNotListed(L"prune-test-container");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_RunningContainerNotPruned)
    {
        // Start a running container, verify prune does NOT remove it
        auto runResult = RunWslc(std::format(L"container run --detach --name prune-running-test {} sleep 300", DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() {
            RunWslc(L"container kill prune-running-test");
            RunWslc(L"container remove --force prune-running-test");
        });

        // Prune should not remove a running container
        const auto pruneResult = RunWslc(L"container prune");
        pruneResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the running container is still present
        VerifyContainerIsListed(L"prune-running-test", L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_MultipleStopped)
    {
        // Create multiple stopped containers and verify all are pruned
        auto create1 = RunWslc(std::format(L"container create --name prune-multi-1 {}", DebianImage.NameAndTag()));
        create1.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId1 = create1.GetStdoutOneLine();

        auto create2 = RunWslc(std::format(L"container create --name prune-multi-2 {}", DebianImage.NameAndTag()));
        create2.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId2 = create2.GetStdoutOneLine();

        auto cleanup = wil::scope_exit([&]() { RunWslc(L"container prune"); });

        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify pruned container IDs are in output
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(containerId1));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(containerId2));

        // Verify both containers are removed
        VerifyContainerIsNotListed(L"prune-multi-1");
        VerifyContainerIsNotListed(L"prune-multi-2");
    }

private:
    const TestImage& DebianImage = DebianTestImage();
};
} // namespace WSLCE2ETests
