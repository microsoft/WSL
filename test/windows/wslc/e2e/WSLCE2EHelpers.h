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
#include <WSLCContainerLauncher.h>
#include "VTSupport.h"

namespace WSLCE2ETests {

// VT sequence constants and helpers for TTY testing.
// Sequences are sourced from wsl::windows::common::vt (VTSupport.h).
namespace VT {
    using namespace wsl::windows::common::vt;

    inline const auto& B_START = Cursor::BracketedPasteOn;
    inline const auto& B_END = Cursor::BracketedPasteOff;
    inline const auto& RESET = Format::Default;
    inline const auto& ERASE_LINE = Erase::LineForward;
    inline const Sequence CR{L"\r"};

    // The shell PS1 uses SGR 1;31 (bold + red) in a single sequence.
    // Sgr({1, 31}) produces L"\x1b[1;31m" to match exactly.
    inline const ConstructedSequence RED = Sgr({1, 31});

    // Prompt pattern used in WSLC TTY sessions.
    inline const std::string SESSION_PROMPT =
        wsl::shared::string::WideToMultiByte(B_START + RED + L"root@ [ " + RESET + L"/" + RED + L" ]# ");

    inline std::string BuildContainerPrompt(const std::string& prompt, bool withBracketedPaste = true)
    {
        if (withBracketedPaste)
        {
            return wsl::shared::string::WideToMultiByte(std::format(L"{}", B_START)) + prompt;
        }
        return prompt;
    }

    inline std::string BuildContainerAttachPrompt(const std::string& prompt)
    {
        return wsl::shared::string::WideToMultiByte(std::format(L"{}{}{}", CR, ERASE_LINE, CR)) + prompt;
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

    IWSLCSession& Session() const
    {
        return *m_session;
    }

private:
    std::wstring m_name;
    std::filesystem::path m_storagePath;
    wil::com_ptr<IWSLCSession> m_session;
};

void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status, const std::wstring& sessionName = L"");
void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);
void VerifyImageIsListed(const TestImage& image);
void VerifyVolumeIsListed(const std::wstring& volumeName);
void VerifyVolumeIsNotListed(const std::wstring& volumeName);
void VerifyNetworkIsListed(const std::wstring& networkName);
void VerifyNetworkIsNotListed(const std::wstring& networkName);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::wslc_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::wslc_schema::InspectImage InspectImage(const std::wstring& imageName);
wsl::windows::common::wslc_schema::InspectVolume InspectVolume(const std::wstring& volumeName);
wsl::windows::common::wslc_schema::Network InspectNetwork(const std::wstring& networkName);
std::vector<wsl::windows::wslc::models::ContainerInformation> ListAllContainers();

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image, const std::wstring& sessionName = L"");
void EnsureImageIsDeleted(const TestImage& image);
void EnsureImageContainersAreDeleted(const TestImage& image);
void EnsureNoUntaggedImages();
void EnsureSessionIsTerminated(const std::wstring& sessionName = L"");
void EnsureVolumeDoesNotExist(const std::wstring& volumeName);
void EnsureNetworkDoesNotExist(const std::wstring& networkName);

void WriteTestFile(const std::filesystem::path& filePath, const std::vector<std::string>& envVariableLines);
void WriteTestFileContent(const std::filesystem::path& filePath, const std::string& content);

// Sets up a clean test directory and returns a scope_exit to remove it.
inline auto SetupTestDirectory(const std::filesystem::path& directory)
{
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    return wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [directory]() {
        std::error_code removeError;
        std::filesystem::remove_all(directory, removeError);
    });
}

std::wstring GetPythonHttpServerScript(uint16_t port);
std::wstring GetPythonUdpEchoServerScript(uint16_t port);

std::string SendUdpAndReceive(uint16_t hostPort, const std::string& payload, const std::string& expectedReply, int family = AF_INET);

void WaitForContainerOutput(const std::wstring& containerName, std::string_view expected, std::chrono::milliseconds timeout = std::chrono::seconds(60));

wsl::windows::common::wslc_schema::Health WaitForContainerHealth(
    const std::wstring& containerName, const std::string_view& expectedStatus, std::chrono::milliseconds timeout = std::chrono::seconds(120));

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

wil::com_ptr<IWSLCSession> OpenDefaultElevatedSession();

void VerifyPseudoConsoleTtySize(WSLCInteractiveSession& session, SHORT columns, SHORT rows);

// Starts a local registry container using the COM API and returns the running container (holds it
// alive) plus the registry address. Host network for plain http, bridge network for tls enabled.
std::pair<wsl::windows::common::RunningWSLCContainer, std::string> StartLocalRegistry(
    IWSLCSession& session,
    const std::string& username = "",
    const std::string& password = "",
    USHORT port = 5000,
    const std::wstring& tlsCertDir = L"");

// Tags an image for a registry and returns the full registry image reference (e.g. "127.0.0.1:PORT/debian:latest").
std::wstring TagImageForRegistry(const std::wstring& imageName, const std::wstring& registryAddress);

// Verifies that a string is a valid hex ID output.
// truncated=true expects 12 hex chars, truncated=false expects 64 hex chars.
inline void VerifyIdOutput(const std::wstring& id, bool truncated)
{
    constexpr size_t c_truncatedLength = 12;
    constexpr size_t c_fullLength = 64;

    const size_t expectedLength = truncated ? c_truncatedLength : c_fullLength;

    VERIFY_ARE_EQUAL(id.size(), expectedLength);

    bool allHex = true;
    for (size_t i = 0; i < expectedLength; i++)
    {
        const auto ch = id[i];
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f')))
        {
            allHex = false;
            break;
        }
    }

    VERIFY_IS_TRUE(allHex, WEX::Common::String().Format(L"ID is not a valid hex string: '%ls'", id.c_str()));
}

} // namespace WSLCE2ETests
