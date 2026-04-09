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
#include <wslc_schema.h>
#include <ContainerModel.h>

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

    // Helper function to build container prompt
    inline std::string BuildContainerPrompt(const std::string& prompt, bool withBracketedPaste = true)
    {
        if (withBracketedPaste)
        {
            return std::format("{}{}", B_START, prompt);
        }
        return std::format("{}", prompt);
    }

    inline std::string BuildContainerAttachPrompt(const std::string& prompt)
    {
        return std::format("{}{}{}{}", CR, ERASE_LINE, CR, prompt);
    }
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

const TestImage& AlpineTestImage();
const TestImage& DebianTestImage();
const TestImage& PythonTestImage();
const TestImage& InvalidTestImage();

struct TestSession
{
    static TestSession Create(const std::wstring& displayName, WSLCNetworkingMode networkingMode = WSLCNetworkingModeNone);

    TestSession(std::wstring name, std::filesystem::path storagePath, wil::com_ptr<IWSLCSession> session) :
        m_name(std::move(name)), m_storagePath(std::move(storagePath)), m_session(std::move(session))
    {
    }

    ~TestSession();

    NON_COPYABLE(TestSession);
    NON_MOVABLE(TestSession);

    const std::wstring& Name() const
    {
        return m_name;
    }

    const std::filesystem::path& StoragePath() const
    {
        return m_storagePath;
    }

private:
    std::wstring m_name;
    std::filesystem::path m_storagePath;
    wil::com_ptr<IWSLCSession> m_session;
};

void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status, const std::wstring& sessionName = L"");
void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::wslc_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::wslc_schema::InspectImage InspectImage(const std::wstring& imageName);
std::vector<wsl::windows::wslc::models::ContainerInformation> ListAllContainers();

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image, const std::wstring& sessionName = L"");
void EnsureImageIsDeleted(const TestImage& image);
void EnsureImageContainersAreDeleted(const TestImage& image);
void EnsureSessionIsTerminated(const std::wstring& sessionName = L"");

void WriteTestFile(const std::filesystem::path& filePath, const std::vector<std::string>& envVariableLines);
std::wstring GetPythonHttpServerScript(uint16_t port);

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
                auto result = RunWslc(L"container list --no-trunc --all");
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