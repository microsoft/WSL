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
        // The offset is 'Z' on a UTC machine and numeric elsewhere, so the timestamp length varies.
        const std::wregex timestampPattern(LR"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}(Z|[+-]\d{2}:\d{2}))");

        const auto separator = line.find(L' ');
        VERIFY_ARE_NOT_EQUAL(std::wstring_view::npos, separator);

        VERIFY_IS_TRUE(std::regex_match(std::wstring{line.substr(0, separator)}, timestampPattern));
        VERIFY_ARE_EQUAL(std::wstring{expectedEvent}, std::wstring{line.substr(separator)});
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
            L"--filter event=create --filter event=start --filter event=die --filter event=destroy",
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
            std::format(L" container die {} (exitCode={}, image={}, name={})", containerId, 128 + WSLCSignalSIGKILL, DebianImage.NameAndTag(), c_eventContainerName));
        VerifyEventLine(lines[3], std::format(L" container destroy {} (image={}, name={})", containerId, DebianImage.NameAndTag(), c_eventContainerName));
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_JsonFormat)
    {
        const auto since = EpochSeconds();
        auto result = RunWslc(std::format(L"container create --name {} {} sleep 60", c_eventContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();

        result = RunWslc(std::format(L"container start {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container kill {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container rm {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"events --since {} --until {} --filter container={} --format json", since, EpochSeconds() + 1, containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const std::vector<std::string> expectedActions{"create", "start", "kill", "die", "destroy"};
        const auto events = ParseNdjsonOutput(result);
        VERIFY_ARE_EQUAL(expectedActions.size(), events.size());

        for (size_t i = 0; i < events.size(); ++i)
        {
            VERIFY_ARE_EQUAL(expectedActions[i], events[i].at("Action").get<std::string>());
        }

        VERIFY_ARE_EQUAL(std::to_string(128 + WSLCSignalSIGKILL), events[3].at("Actor").at("Attributes").at("exitCode").get<std::string>());

        for (const auto& event : events)
        {
            VERIFY_ARE_EQUAL(std::string{"container"}, event.at("Type").get<std::string>());
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(containerId), event.at("Actor").at("ID").get<std::string>());
            VERIFY_ARE_EQUAL(
                wsl::shared::string::WideToMultiByte(c_eventContainerName),
                event.at("Actor").at("Attributes").at("name").get<std::string>());
            VERIFY_ARE_EQUAL(std::string{"local"}, event.at("scope").get<std::string>());

            VERIFY_ARE_EQUAL(event.at("Action").get<std::string>(), event.at("status").get<std::string>());
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(containerId), event.at("id").get<std::string>());
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(DebianImage.NameAndTag()), event.at("from").get<std::string>());

            const auto timeNano = event.at("timeNano").get<std::int64_t>();
            VERIFY_IS_GREATER_THAN(timeNano, 0LL);
            VERIFY_ARE_EQUAL(timeNano / 1'000'000'000, event.at("time").get<std::int64_t>());
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_ContainerHealth)
    {
        const auto since = EpochSeconds();
        auto result = RunWslc(std::format(
            LR"(container create --health-cmd "test -f /tmp/healthy" --health-interval 1s --health-timeout 3s --health-retries 1 --name {} {} sleep infinity)",
            c_eventContainerName,
            DebianImage.NameAndTag()));

        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containerId = result.GetStdoutOneLine();
        const auto filters = std::format(
            LR"(--filter type=container --filter container={} --filter event=create --filter event=start --filter event=stop --filter "event=health_status: healthy" --filter "event=health_status: unhealthy")",
            containerId);

        auto events = RunWslcInteractive(
            std::format(L"events --since {} {}", since, filters), ElevationType::Elevated, std::nullopt, ProcessGroup::Create);

        auto stopReader = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (events.IsRunning())
            {
                events.SendCtrlBreak();
            }
        });

        const auto expectedEvent = [&](std::wstring_view action) {
            return std::format(L" container {} {} (image={}, name={})", action, containerId, DebianImage.NameAndTag(), c_eventContainerName);
        };

        const auto createEvent = expectedEvent(L"create");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(createEvent));

        RunWslc(std::format(L"container start {}", containerId)).Verify({.Stderr = L"", .ExitCode = 0});
        const auto startEvent = expectedEvent(L"start");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(startEvent));

        const auto unhealthyEvent = expectedEvent(L"health_status: unhealthy");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(unhealthyEvent));
        VerifyContainerIsListed(containerId, L"running");

        RunWslc(std::format(L"container exec {} touch /tmp/healthy", containerId)).Verify({.Stderr = L"", .ExitCode = 0});

        const auto healthyEvent = expectedEvent(L"health_status: healthy");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(healthyEvent));
        VerifyContainerIsListed(containerId, L"running");

        RunWslc(std::format(L"container stop {} -t 0", containerId)).Verify({.Stderr = L"", .ExitCode = 0});
        const auto stopEvent = expectedEvent(L"stop");
        WaitForPseudoConsoleOutput(events, wsl::shared::string::WideToMultiByte(stopEvent));
        VerifyContainerIsListed(containerId, L"exited");

        stopReader.reset();
        VERIFY_ARE_EQUAL(0, events.Wait());

        events.VerifyNoErrors();

        const WSLCExecutionResult output{.Stdout = wsl::shared::string::MultiByteToWide(events.GetStdoutData())};
        const auto lines = output.GetStdoutLines();
        const std::vector<std::wstring> expected{createEvent, startEvent, unhealthyEvent, healthyEvent, stopEvent};
        VERIFY_ARE_EQUAL(expected.size(), lines.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            VerifyEventLine(lines[index], expected[index]);
        }

        result = RunWslc(std::format(L"events --since {} --until {} {} --format json", since, EpochSeconds() + 1, filters));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto history = ParseNdjsonOutput(result);

        const std::vector<std::string> expectedActions{
            "create", "start", "health_status: unhealthy", "health_status: healthy", "stop"};
        VERIFY_ARE_EQUAL(expectedActions.size(), history.size());

        for (size_t index = 0; index < history.size(); ++index)
        {
            const auto& event = history[index];
            VERIFY_ARE_EQUAL(expectedActions[index], event.at("Action").get<std::string>());
            VERIFY_ARE_EQUAL(std::string{"container"}, event.at("Type").get<std::string>());
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(containerId), event.at("Actor").at("ID").get<std::string>());

            const auto& attributes = event.at("Actor").at("Attributes");
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(c_eventContainerName), attributes.at("name").get<std::string>());
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(DebianImage.NameAndTag()), attributes.at("image").get<std::string>());
            VERIFY_IS_FALSE(attributes.contains("exitCode"));

            VERIFY_ARE_EQUAL(event.at("Action").get<std::string>(), event.at("status").get<std::string>());

            const auto timeNano = event.at("timeNano").get<std::int64_t>();
            VERIFY_IS_GREATER_THAN_OR_EQUAL(timeNano, since * 1'000'000'000);
            VERIFY_ARE_EQUAL(timeNano / 1'000'000'000, event.at("time").get<std::int64_t>());
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_InvalidFormatOption)
    {
        auto result = RunWslc(L"events --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
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

        stopReader.reset();
        VERIFY_ARE_EQUAL(0, events.Wait());
        events.VerifyNoErrors();

        const WSLCExecutionResult output{.Stdout = wsl::shared::string::MultiByteToWide(events.GetStdoutData())};
        const auto lines = output.GetStdoutLines();
        const std::vector<std::wstring> expected{createEvent, connectEvent, disconnectEvent, destroyEvent};
        VERIFY_ARE_EQUAL(expected.size(), lines.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            VerifyEventLine(lines[index], expected[index]);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Events_NetworkJsonFormat)
    {
        GUID runId{};
        VERIFY_SUCCEEDED(CoCreateGuid(&runId));
        const auto suffix = wsl::shared::string::GuidToString<wchar_t>(runId, wsl::shared::string::GuidToStringFlags::None);
        const auto networkName = L"wslc-events-json-network-" + suffix;
        const auto containerName = L"wslc-events-json-container-" + suffix;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            EnsureContainerDoesNotExist(containerName);
            EnsureNetworkDoesNotExist(networkName);
        });

        const auto since = EpochSeconds();
        auto result = RunWslc(std::format(L"network create --driver bridge {}", networkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto networkId = InspectNetwork(networkName).Id;

        result = RunWslc(std::format(
            L"container run -d --name {} --network {} {} sleep infinity", containerName, networkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = wsl::shared::string::WideToMultiByte(result.GetStdoutOneLine());

        result = RunWslc(std::format(L"container rm -f {}", containerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network rm {}", networkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Network removal doesn't wait for its event to be recorded, so retry until the whole lifecycle is visible.
        std::vector<nlohmann::json> events;
        const auto queryEvents = [&]() {
            const auto until = EpochSeconds() + 1;
            const auto query = RunWslc(std::format(
                L"events --since {} --until {} --filter type=network "
                L"--filter network={} --format json",
                since,
                until,
                networkName));
            THROW_HR_IF(E_FAIL, query.ExitCode != 0u);

            events = ParseNdjsonOutput(query);
            THROW_HR_IF(E_ABORT, events.size() < 4);
        };

        const auto retryPeriod = std::chrono::milliseconds(200);
        const auto timeout = std::chrono::seconds(30);
        VERIFY_NO_THROW(wsl::shared::retry::RetryWithTimeout<void>(queryEvents, retryPeriod, timeout));

        const std::vector<std::string> expectedActions{"create", "connect", "disconnect", "destroy"};
        VERIFY_ARE_EQUAL(expectedActions.size(), events.size());

        for (size_t index = 0; index < events.size(); ++index)
        {
            const auto& event = events[index];
            const auto action = event.at("Action").get<std::string>();
            VERIFY_ARE_EQUAL(expectedActions[index], action);
            VERIFY_ARE_EQUAL(std::string{"network"}, event.at("Type").get<std::string>());
            VERIFY_ARE_EQUAL(networkId, event.at("Actor").at("ID").get<std::string>());

            const auto& attributes = event.at("Actor").at("Attributes");
            VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(networkName), attributes.at("name").get<std::string>());
            VERIFY_ARE_EQUAL(std::string{"bridge"}, attributes.at("type").get<std::string>());

            const bool endpointEvent = action == "connect" || action == "disconnect";
            VERIFY_ARE_EQUAL(endpointEvent, attributes.contains("container"));
            if (endpointEvent)
            {
                VERIFY_ARE_EQUAL(containerId, attributes.at("container").get<std::string>());
            }

            VERIFY_ARE_EQUAL(std::string{"local"}, event.at("scope").get<std::string>());

            const auto timeNano = event.at("timeNano").get<std::int64_t>();
            VERIFY_IS_GREATER_THAN(timeNano, 0LL);
            VERIFY_ARE_EQUAL(timeNano / 1'000'000'000, event.at("time").get<std::int64_t>());

            // Docker only reports its legacy fields for container events.
            VERIFY_IS_FALSE(event.contains("status"));
            VERIFY_IS_FALSE(event.contains("id"));
            VERIFY_IS_FALSE(event.contains("from"));
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
