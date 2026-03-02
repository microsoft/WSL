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
#include "WSLCCommand.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;

std::wstring GetStdoutOneLine(const WSLCExecutionResult& result)
{
    auto stdoutLines = result.GetStdoutLines();
    VERIFY_ARE_EQUAL(1u, stdoutLines.size());
    return stdoutLines[0];
}

void VerifyContainerIsNotListed(const std::wstring& containerNameOrId)
{
    auto result = WSLCCommand::ContainerList("--all");
    result.VerifyNoErrors();

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message =
                L"Container '" + containerNameOrId + L"' found in container list output but it should not be listed";
            VERIFY_FAIL(message.c_str());
        }
    }
}

void VerifyContainerIsListed(const std::wstring& containerNameOrId, const std::wstring& status)
{
    auto result = WSLCCommand::ContainerList("--all");
    result.VerifyNoErrors();

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message = L"Container '" + containerNameOrId + L"' found in container list output but status '" +
                                         status + L"' was not found in the same line";
            VERIFY_IS_TRUE(line.find(status) != std::wstring::npos, message.c_str());
            return;
        }
    }

    const std::wstring message = L"Container '" + containerNameOrId + L"' not found in container list output";
    VERIFY_FAIL(message.c_str());
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    auto listResult = WSLCCommand::ContainerList("--all");
    listResult.VerifyNoErrors();

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        if (line.find(containerName) != std::wstring::npos)
        {
            auto result = WSLCCommand::ContainerDelete(containerName, L"--force");
            result.VerifyNoErrors();
            break;
        }
    }
}
} // namespace WSLCE2ETests