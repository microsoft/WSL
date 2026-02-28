/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutorHelpers.h

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCExecutorHelpers.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;

std::wstring GetStdoutOneLine(const WSLCExecutionResult& result)
{
    auto stdoutLines = result.GetStdoutLines();
    VERIFY_ARE_EQUAL(1u, stdoutLines.size());
    return stdoutLines[0];
}

void EnsureContainerIsNotListed(const std::wstring& containerNameOrId)
{
    auto result = WSLCExecutor::Execute(L"container list --all");
    VERIFY_ARE_EQUAL(L"", result.Stderr);
    VERIFY_ARE_EQUAL(S_OK, result.ExitCode);
    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message = L"Container '" + containerNameOrId + L"' found in container list output but it should not be listed";
            VERIFY_FAIL(message.c_str());
        }
    }
}

void EnsureContainerIsListed(const std::wstring& containerNameOrId, const std::wstring& status)
{
    auto result = WSLCExecutor::Execute(L"container list --all");
    VERIFY_ARE_EQUAL(L"", result.Stderr);
    VERIFY_ARE_EQUAL(S_OK, result.ExitCode);
    auto outputLines = result.GetStdoutLines();

    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message =
                L"Container '" + containerNameOrId + L"' found in container list output but status '" + status +
                L"' was not found in the same line";
            VERIFY_IS_TRUE(line.find(status) != std::wstring::npos, message.c_str());
            return;
        }
    }

    const std::wstring message = L"Container '" + containerNameOrId + L"' not found in container list output";
    VERIFY_FAIL(message.c_str());
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    auto listResult = WSLCExecutor::Execute(L"container list --all");
    VERIFY_ARE_EQUAL(L"", listResult.Stderr);
    VERIFY_ARE_EQUAL(S_OK, listResult.ExitCode);
    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        if (line.find(containerName) != std::wstring::npos)
        {
            auto deleteCommand = L"container delete " + containerName + L" --force";
            WSLCExecutor::ExecuteAndVerify(deleteCommand, L"", L"", S_OK);
            break;
        }
    }
}
} // namespace WSLCE2ETests