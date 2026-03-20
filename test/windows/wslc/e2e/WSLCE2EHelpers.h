/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.h

Abstract:

    This file contains helper functions for WSLCE2E tests.
--*/

#pragma once

#include "WSLCExecutor.h"
#include <docker_schema.h>
#include <chrono>
#include <wsla_schema.h>

namespace WSLCE2ETests {

// VT100/ANSI escape sequence constants for TTY testing
namespace VT {
// Bracketed paste mode control sequences
#define VT_B_START "\x1b[?2004h" // Enable bracketed paste mode
#define VT_B_END "\x1b[?2004l"   // Disable bracketed paste mode

// Color/formatting sequences
#define VT_RESET "\x1b[0m"  // Reset all attributes
#define VT_RED "\x1b[1;31m" // Bold red text

// Terminal control sequences
#define VT_ERASE_LINE "\x1b[K" // Erase from cursor to end of line
#define VT_CR "\r"             // Carriage return

    // Prompt patterns used in WSLC.
    constexpr auto SESSION_PROMPT = VT_B_START VT_RED "root@ [ " VT_RESET "/" VT_RED " ]# ";
    constexpr auto CONTAINER_PROMPT = VT_B_START "root@:/# ";
    constexpr auto CONTAINER_ATTACH_PROMPT = VT_CR VT_ERASE_LINE VT_CR "root@:/# ";

    // Constexpr representations of the control sequences for use in tests.
    constexpr auto B_START = VT_B_START;
    constexpr auto B_END = VT_B_END;
    constexpr auto RESET = VT_RESET;
    constexpr auto RED = VT_RED;
    constexpr auto ERASE_LINE = VT_ERASE_LINE;
    constexpr auto CR = VT_CR;

// Remove macros to avoid polluting global namespace.
#undef VT_B_START
#undef VT_B_END
#undef VT_RESET
#undef VT_RED
#undef VT_ERASE_LINE
#undef VT_CR
} // namespace VT

struct TestImage
{
    std::wstring Name;
    std::wstring Tag;
    std::filesystem::path Path;
    std::wstring NameAndTag() const
    {
        return std::format(L"{}:{}", Name, Tag);
    }
};

const TestImage& DebianTestImage();
const TestImage& InvalidTestImage();

void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::wsla_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::wsla_schema::InspectImage InspectImage(const std::wstring& imageName);

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image);
void EnsureImageIsDeleted(const TestImage& image);

// Default timeout of 0 will execute once.
template <typename IntervalRep, typename IntervalPeriod, typename TimeoutRep, typename TimeoutPeriod>
void VerifyContainerIsNotListed(
    const std::wstring& containerNameOrId,
    std::chrono::duration<IntervalRep, IntervalPeriod> retryInterval,
    std::chrono::duration<TimeoutRep, TimeoutPeriod> timeout)
{
    try
    {
        wsl::shared::retry::RetryWithTimeout<void>(
            [&containerNameOrId]() {
                auto result = RunWslc(L"container list --all");
                result.Verify({.Stderr = L"", .ExitCode = 0});

                auto outputLines = result.GetStdoutLines();
                for (const auto& line : outputLines)
                {
                    if (line.find(containerNameOrId) != std::wstring::npos)
                    {
                        THROW_HR(E_FAIL);
                    }
                }
            },
            retryInterval,
            timeout);
    }
    catch (...)
    {
        HRESULT hr = wil::ResultFromCaughtException();
        const bool hasTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count() > 0;
        const std::wstring message =
            hr == E_FAIL
                ? std::format(L"Container '{}' found in container list output{}", containerNameOrId, hasTimeout ? L" after timeout" : L" but it should not be listed")
                : std::format(
                      L"Unexpected error while verifying container '{}' is not listed: 0x{:08X}",
                      containerNameOrId,
                      static_cast<unsigned int>(hr));
        VERIFY_FAIL(message.c_str());
    }
}

inline void VerifyContainerIsNotListed(const std::wstring& containerNameOrId)
{
    VerifyContainerIsNotListed(containerNameOrId, std::chrono::milliseconds(0), std::chrono::milliseconds(0));
}
} // namespace WSLCE2ETests