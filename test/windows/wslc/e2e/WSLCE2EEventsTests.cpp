/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EEventsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC events.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {

namespace {

    constexpr auto c_eventContainerName = L"wslc-events-test";
    constexpr size_t c_eventTimestampLength = 35;

    LONGLONG EpochSeconds()
    {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count();
    }

    void WaitForNextSecond()
    {
        const auto nextSecond = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) + std::chrono::seconds{1};
        std::this_thread::sleep_until(nextSecond);
    }

    void VerifyEventLine(std::wstring_view line, std::wstring_view expectedEvent)
    {
        VERIFY_ARE_EQUAL(c_eventTimestampLength + expectedEvent.size(), line.size());
        if (line.size() < c_eventTimestampLength)
        {
            return;
        }

        VERIFY_ARE_EQUAL(L'-', line[4]);
        VERIFY_ARE_EQUAL(L'-', line[7]);
        VERIFY_ARE_EQUAL(L'T', line[10]);
        VERIFY_ARE_EQUAL(L':', line[13]);
        VERIFY_ARE_EQUAL(L':', line[16]);
        VERIFY_ARE_EQUAL(L'.', line[19]);
        VERIFY_IS_TRUE(line[29] == L'+' || line[29] == L'-');
        VERIFY_ARE_EQUAL(L':', line[32]);
        VERIFY_ARE_EQUAL(std::wstring{expectedEvent}, std::wstring{line.substr(c_eventTimestampLength)});
    }

} // namespace

class WSLCE2EEventsTests
{
    WSLC_TEST_CLASS(WSLCE2EEventsTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(c_eventContainerName);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(c_eventContainerName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_LiveOutputAndCancellation)
    {
        auto result = RunWslc(std::format(L"container create --name {} {} sleep 60", c_eventContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        WaitForNextSecond();
        const auto since = EpochSeconds();

        auto events = RunWslcInteractive(
            std::format(L"events --since {} --filter container={}", since, containerId), ElevationType::Elevated, std::nullopt, ProcessGroup::Create);

        result = RunWslc(std::format(L"container start {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto expectedEvent =
            std::format(L" container start {} (image={}, name={})", containerId, DebianImage.NameAndTag(), c_eventContainerName);
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(expectedEvent));

        events.SendCtrlBreak();
        VERIFY_ARE_EQUAL(0, events.Wait());
        events.VerifyNoErrors();

        auto output = wsl::shared::string::MultiByteToWide(events.GetStdoutData());
        VERIFY_IS_TRUE(output.ends_with(L'\n'));
        if (output.empty())
        {
            return;
        }

        output.pop_back();
        if (output.ends_with(L'\r'))
        {
            output.pop_back();
        }

        VERIFY_ARE_EQUAL(std::wstring::npos, output.find(L'\n'));
        VerifyEventLine(output, expectedEvent);
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_DefaultExcludesBufferedEvents)
    {
        auto result = RunWslc(std::format(L"container create --name {} {} sleep 60", c_eventContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        WaitForNextSecond();

        result = RunWslc(std::format(L"events --until {} --filter container={}", EpochSeconds() + 1, containerId));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_HistoricalTimeAndFilterCombinations)
    {
        const auto since = EpochSeconds();
        auto result = RunWslc(std::format(L"container create --name {} {} sleep 60", c_eventContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        result = RunWslc(std::format(L"container start {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container kill {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"exited");

        result = RunWslc(std::format(L"container rm {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsNotListed(containerId);

        const auto until = EpochSeconds() + 1;
        result = RunWslc(std::format(
            L"events --since {} --until {} --filter type=container --filter container={} --filter image={} "
            L"--filter event=create --filter event=start --filter event=stop --filter event=destroy",
            since,
            until,
            containerId,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto lines = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(4u, lines.size());
        VerifyEventLine(lines[0], std::format(L" container create {} (image={}, name={})", containerId, DebianImage.NameAndTag(), c_eventContainerName));
        VerifyEventLine(lines[1], std::format(L" container start {} (image={}, name={})", containerId, DebianImage.NameAndTag(), c_eventContainerName));
        VerifyEventLine(
            lines[2],
            std::format(L" container stop {} (exitCode={}, image={}, name={})", containerId, 128 + WSLCSignalSIGKILL, DebianImage.NameAndTag(), c_eventContainerName));
        VerifyEventLine(lines[3], std::format(L" container destroy {} (image={}, name={})", containerId, DebianImage.NameAndTag(), c_eventContainerName));
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_NetworkLifecycle)
    {
        GUID runId{};
        VERIFY_SUCCEEDED(CoCreateGuid(&runId));
        const auto suffix = wsl::shared::string::GuidToString<wchar_t>(runId, wsl::shared::string::GuidToStringFlags::None);
        const auto networkName = L"wslc-events-network-" + suffix;
        const auto containerName = L"wslc-events-container-" + suffix;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            EnsureContainerDoesNotExist(containerName);
            EnsureNetworkDoesNotExist(networkName);
        });

        auto events = RunWslcInteractive(
            std::format(L"events --since 0 --filter type=network --filter network={}", networkName),
            ElevationType::Elevated,
            std::nullopt,
            ProcessGroup::Create);
        auto stopReader = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (events.IsRunning())
            {
                events.SendCtrlBreak();
            }
        });

        auto result = RunWslc(std::format(L"network create --driver bridge {}", networkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto networkId = wsl::shared::string::MultiByteToWide(InspectNetwork(networkName).Id);

        const auto expectedEvent = [&](std::wstring_view action, std::wstring_view containerId = {}) {
            const auto containerAttribute = containerId.empty() ? std::wstring{} : std::format(L"container={}, ", containerId);
            return std::format(L" network {} {} ({}name={}, type=bridge)", action, networkId, containerAttribute, networkName);
        };

        const auto createEvent = expectedEvent(L"create");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(createEvent));

        result = RunWslc(std::format(
            L"container run -d --name {} --network {} {} sleep infinity", containerName, networkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        const auto connectEvent = expectedEvent(L"connect", containerId);
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(connectEvent));

        result = RunWslc(std::format(L"container rm -f {}", containerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto disconnectEvent = expectedEvent(L"disconnect", containerId);
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(disconnectEvent));

        result = RunWslc(std::format(L"network rm {}", networkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto destroyEvent = expectedEvent(L"destroy");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(destroyEvent));

        events.SendCtrlBreak();
        VERIFY_ARE_EQUAL(0, events.Wait());
        events.VerifyNoErrors();
        stopReader.release();

        const WSLCExecutionResult output{.Stdout = wsl::shared::string::MultiByteToWide(events.GetStdoutData())};
        const auto lines = output.GetStdoutLines();
        const std::vector<std::wstring> expected{createEvent, connectEvent, disconnectEvent, destroyEvent};
        VERIFY_ARE_EQUAL(expected.size(), lines.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            VerifyEventLine(lines[index], expected[index]);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_RejectsUnsupportedFilter)
    {
        auto session = OpenDefaultElevatedSession();
        for (const auto* command : {L"events", L"system events"})
        {
            for (const auto* key : {L"unsupported", L"label", L""})
            {
                const auto result = RunWslc(std::format(L"{} --filter {}=test", command, key));
                result.Verify({.Stdout = L"", .ExitCode = 1});
                VERIFY_IS_TRUE(result.StderrContainsSubstring(wsl::shared::Localization::MessageWslcInvalidFilter(key)));
                VERIFY_IS_TRUE(result.StderrContainsSubstring(L"E_INVALIDARG"));
            }
        }
    }

private:
    const TestImage& DebianImage = DebianTestImage();
};

} // namespace WSLCE2ETests
