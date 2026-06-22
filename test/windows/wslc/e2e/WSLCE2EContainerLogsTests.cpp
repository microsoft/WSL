/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerLogsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container logs.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerLogsTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerLogsTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Tail)
    {
        // Run a container that outputs two lines
        auto result = RunWslc(std::format(
            L"container run --name {} {} sh -c \"echo line1 && echo line2\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"line1\nline2\n", .Stderr = L"", .ExitCode = 0});

        // Verify --tail 1 only shows the last line
        result = RunWslc(std::format(L"container logs --tail 1 {}", WslcContainerName));
        result.Verify({.Stdout = L"line2\n", .Stderr = L"", .ExitCode = 0});

        // Verify -n 2 shows both lines
        result = RunWslc(std::format(L"container logs -n 2 {}", WslcContainerName));
        result.Verify({.Stdout = L"line1\nline2\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Timestamps)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo hello\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify --timestamps adds timestamp prefix to log lines
        result = RunWslc(std::format(L"container logs --timestamps {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Timestamps should be in RFC3339 format (e.g. "2024-01-01T00:00:00.000000000Z hello")
        // Verify output contains a timestamp-like prefix followed by the message
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"hello"));
        auto lines = result.GetStdoutLines();
        VERIFY_IS_TRUE(!lines.empty());
        // Validate RFC3339 structure: YYYY-MM-DDTHH:MM:SS at the start of the line
        VERIFY_IS_TRUE(lines[0].size() >= 20);
        VERIFY_ARE_EQUAL(lines[0][4], L'-');
        VERIFY_ARE_EQUAL(lines[0][7], L'-');
        VERIFY_ARE_EQUAL(lines[0][10], L'T');
        VERIFY_ARE_EQUAL(lines[0][13], L':');
        VERIFY_ARE_EQUAL(lines[0][16], L':');
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_TimestampsShortFlag)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo world\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify -t (short for --timestamps) works the same way
        result = RunWslc(std::format(L"container logs -t {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"world"));
        auto lines = result.GetStdoutLines();
        VERIFY_IS_TRUE(!lines.empty());
        VERIFY_IS_TRUE(lines[0].size() >= 20);
        VERIFY_ARE_EQUAL(lines[0][4], L'-');
        VERIFY_ARE_EQUAL(lines[0][7], L'-');
        VERIFY_ARE_EQUAL(lines[0][10], L'T');
        VERIFY_ARE_EQUAL(lines[0][13], L':');
        VERIFY_ARE_EQUAL(lines[0][16], L':');
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Since)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo before\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Using --since 0 should return all logs (epoch start)
        result = RunWslc(std::format(L"container logs --since 0 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"before"));

        // Using --since with a far-future timestamp should return no logs
        result = RunWslc(std::format(L"container logs --since 9999999999 {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_SinceRfc3339)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo rfc3339test\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Using --since with an RFC3339 timestamp in the past should return all logs
        result = RunWslc(std::format(L"container logs --since 2000-01-01T00:00:00Z {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"rfc3339test"));

        // Using --since with an RFC3339 timestamp far in the future should return no logs
        result = RunWslc(std::format(L"container logs --since 2099-12-31T23:59:59Z {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Using --since with an RFC3339 timestamp with timezone offset
        result = RunWslc(std::format(L"container logs --since 2000-01-01T00:00:00+00:00 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"rfc3339test"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Until)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo test_until\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Using --until with a far-future timestamp should return all logs
        result = RunWslc(std::format(L"container logs --until 9999999999 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"test_until"));

        // Using --until 1 (epoch + 1 second) should return no logs since container was created much later
        result = RunWslc(std::format(L"container logs --until 1 {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_SinceAndUntilCombined)
    {
        // Run a container that outputs a line
        auto result =
            RunWslc(std::format(L"container run --name {} {} sh -c \"echo combined\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Using --since 0 --until far-future should return all logs
        result = RunWslc(std::format(L"container logs --since 0 --until 9999999999 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"combined"));

        // Using --since far-future --until far-future should return no logs
        result = RunWslc(std::format(L"container logs --since 9999999999 --until 9999999999 {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_AllOptionsCombined)
    {
        // Run a container that outputs multiple lines
        auto result = RunWslc(std::format(
            L"container run --name {} {} sh -c \"echo a && echo b && echo c\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Combine --timestamps --since 0 --tail 2
        result = RunWslc(std::format(L"container logs --timestamps --since 0 --tail 2 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Should have at most 2 lines (from --tail 2) and each should have a timestamp
        auto lines = result.GetStdoutLines();
        VERIFY_IS_TRUE(lines.size() <= 2);
        for (const auto& line : lines)
        {
            if (!line.empty())
            {
                // Validate RFC3339 structure: YYYY-MM-DDTHH:MM:SS at the start of the line
                VERIFY_IS_TRUE(line.size() >= 20);
                VERIFY_ARE_EQUAL(line[4], L'-');
                VERIFY_ARE_EQUAL(line[7], L'-');
                VERIFY_ARE_EQUAL(line[10], L'T');
                VERIFY_ARE_EQUAL(line[13], L':');
                VERIFY_ARE_EQUAL(line[16], L':');
            }
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Follow)
    {
        // Sleep between the two lines so the second is not produced until after line 1 is read.
        constexpr int InterLineSleepSeconds = 2;
        auto result = RunWslc(std::format(
            L"container run -d --name {} {} sh -c \"echo follow-line-1; sleep {}; echo follow-line-2\"",
            WslcContainerName,
            DebianImage.NameAndTag(),
            InterLineSleepSeconds));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto logsSession = RunWslcInteractive(std::format(L"container logs -f {}", WslcContainerName));

        // If --follow buffered, line 1 would arrive only after the container exited.
        logsSession.ExpectStdout("follow-line-1\n");
        VERIFY_IS_TRUE(logsSession.IsRunning(), L"`logs -f` should still be running mid-sleep after line 1");

        logsSession.ExpectStdout("follow-line-2\n");

        auto exitCode = logsSession.Wait(30000);
        VERIFY_ARE_EQUAL(0, exitCode);
        logsSession.VerifyNoErrors();
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-logs";
    const TestImage& DebianImage = DebianTestImage();
};

} // namespace WSLCE2ETests
