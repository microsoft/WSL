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
        EnsureContainerDoesNotExist(L"prune-confirm-test");
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        // Clean up any leftover containers
        RunWslc(L"container prune --force");
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
        RunWslc(L"container prune --force").Verify({.Stderr = L"", .ExitCode = 0});

        const auto result = RunWslc(L"container prune --force");

        // With nothing pruned docker emits only the reclaimed-space line, with no header or leading blank line.
        result.Verify({.Stdout = L"Total reclaimed space: 0B\r\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_StoppedContainer)
    {
        // Create and stop a container, then prune it
        auto createResult = RunWslc(std::format(L"container create --name prune-test-container {}", DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = createResult.GetStdoutOneLine();

        auto cleanup = wil::scope_exit([&]() { RunWslc(L"container prune --force"); });

        // The created container is in stopped state, so prune should remove it
        const auto result = RunWslc(L"container prune --force");
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
        const auto pruneResult = RunWslc(L"container prune --force");
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

        auto cleanup = wil::scope_exit([&]() { RunWslc(L"container prune --force"); });

        const auto result = RunWslc(L"container prune --force");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify pruned container IDs are in output
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(containerId1));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(containerId2));

        // Verify both containers are removed
        VerifyContainerIsNotListed(L"prune-multi-1");
        VerifyContainerIsNotListed(L"prune-multi-2");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_WithoutForcePromptsAndDeclines)
    {
        // Without --force the prune asks for confirmation. No input is attached here, so the read hits
        // end of input and declines: the container survives and the command still succeeds.
        RunWslc(std::format(L"container create --name {} {}", ConfirmContainerName, DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() { EnsureContainerDoesNotExist(ConfirmContainerName); });

        // Argument validation runs before the confirmation prompt, so a bad filter fails with empty
        // stdout rather than printing the warning first.
        const auto malformed = RunWslc(L"container prune --filter label");
        malformed.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(malformed.StderrContainsSubstring(Localization::WSLCCLI_InvalidFilterError(L"label")));

        const auto result = RunWslc(L"container prune");
        result.Verify({.ExitCode = 0});

        // The warning goes to stderr; the question goes to stdout.
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_ContainerPruneConfirm()));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_PruneConfirmPrompt()));

        // Declining prunes nothing, so no deleted header is emitted and the container is still listed.
        VERIFY_IS_FALSE(result.StdoutContainsSubstring(Localization::WSLCCLI_ContainerPruneDeletedHeader()));
        VerifyContainerIsListed(ConfirmContainerName, L"created");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_FilterExcludesNonMatchingContainer)
    {
        // container prune gained --filter here, so a label filter that matches nothing must leave the
        // stopped container in place, and an unfiltered prune must still remove it.
        auto createResult = RunWslc(std::format(L"container create --name {} {}", ConfirmContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = createResult.GetStdoutOneLine();

        auto cleanup = wil::scope_exit([&]() { EnsureContainerDoesNotExist(ConfirmContainerName); });

        const auto filtered = RunWslc(L"container prune --force --filter label=wslc.test.never=present");
        filtered.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(filtered.StdoutContainsSubstring(containerId));
        VerifyContainerIsListed(ConfirmContainerName, L"created");

        const auto unfiltered = RunWslc(L"container prune --force");
        unfiltered.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(unfiltered.StdoutContainsSubstring(containerId));
        VerifyContainerIsNotListed(ConfirmContainerName);
    }

private:
    const std::wstring ConfirmContainerName = L"prune-confirm-test";
    const TestImage& DebianImage = DebianTestImage();
};
} // namespace WSLCE2ETests
